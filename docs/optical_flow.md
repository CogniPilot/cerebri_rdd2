# RDD2 optical-flow flight profile

The optical-flow profile keeps the normal onboard M10 GPS, CRSF, streaming
ICM45686, gPTP, and CSyn/Zenoh configuration. It additionally subscribes to
the fixed-layout Synapse `flow_vel` topic produced by the shaped sensor node
and fuses its planar velocity into `Vehicles.Rdd2.NavigationEstimator`.

Build and flash the isolated profile with:

```sh
nix run path:.#west-update
nix run path:.#build-optical-flow
nix run path:.#flash-optical-flow
```

The build directory defaults to `build-mr_vmu_tropic-optical-flow`, so it
cannot accidentally replace the ordinary GPS-only build. Both commands verify
that the result is a signed MCUboot flight image, that CRSF and the streaming
IMU remain enabled, and that `CONFIG_RDD2_OPTICAL_FLOW_SOURCE_CSYN=y` is in the
application configuration.

## Sensor-node contract

The producer publishes `synapse.topic.OpticalFlowVelocityData` on the
canonical `flow_vel` key. The VMU accepts a sample only when:

- sensor id is 0 (configurable with `RDD2_OPTICAL_FLOW_SENSOR_ID`);
- source timestamps are nonzero and strictly increasing;
- `VelocityValid`, `TiltCompensated`, and `RangeTrusted` bits are all set;
- time status is `GptpSynced` or `GptpHoldover`;
- velocity, distance, roll, and pitch are finite and within the configured
  speed, range, and tilt envelopes; and
- quality is at least 100 out of 255.

The velocity is `{forward,left}` in body FLU axes after the producer has
applied camera mounting, range, and tilt compensation. The consumer maps
quality monotonically to diagonal velocity covariance: the defaults range
from 1.0 m/s standard deviation at minimum quality to 0.1 m/s at quality 255.
It holds the last accepted value for at most 150 ms, marks only a newly
accepted timestamp as fresh, and then withdraws the measurement. Replayed or
out-of-order timestamps cannot refresh the deadline.

The source timestamp is used to prove producer ordering. Estimator time uses
the local IMU control clock at receipt because gPTP and the IMU sensor clock
have different epochs.

## Prop-off bench gate

Before flight, confirm the shaped node and VMU are on the same gPTP domain,
then use the RDD2 shell to watch `optical_flow_velocity`. Verify that timestamp
and generation advance at the sensor rate; the three validity bits stay set;
quality, distance, and velocity respond to motion; and stopping the producer
withdraws estimator freshness within 150 ms. A successful build alone does
not qualify the sensor mounting, axis signs, scale, range compensation, or
network timing.

Use `optical_flow status` to distinguish transport acceptance from estimator
fusion. The first line reports the adapter state and accepted-sample age. The
second reports accepted/rejected producer counts and the raw measurement. The
third reports the durable fusion count, last fusion control timestamp,
estimator rejection count, correction outcome/source, and recovery stage.
Before arming, require:

- `source_generation` and `accepted` to advance at the producer rate without a
  growing `rejected` count;
- `fused_count` to advance while the vehicle is translated over a textured
  surface;
- forward motion to produce positive body-FLU X velocity and leftward motion
  to produce positive body-FLU Y velocity;
- measured speed scale to remain consistent at two known travel distances and
  at two sensor heights within the configured range; and
- stopping the producer to report `expired` within 150 ms without degrading
  GPS origin validity or estimator recovery state.

The initial outdoor progression keeps GPS as the position anchor: perform a
manual ATTITUDE hover and GPS position hold first, then enable optical-flow
fusion during a low, slow hover. Cover or stop the flow sensor and prove clean
GPS-only continuation before flying a 0.5--1 m square. Optical flow is not an
independent position or landing source in this profile.
