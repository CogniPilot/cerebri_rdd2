# RDD2 lockstep adapter

This directory contains only RDD2-owned integration: the vehicle's generated
Synapse payload layout, IMU/manual-control ingestion, controller topic handoff,
and native/FastDyn adapters. It owns `rdd2_fastdyn_lockstep_shared` because the
storage and ABI are vehicle-specific.

The reusable sequencing implementation lives in the `cerebri_modules` west
module and is consumed through `<cerebri_lockstep/sequence.h>`. Do not duplicate
generation, wait/respond, termination, or memory-ordering logic here.
