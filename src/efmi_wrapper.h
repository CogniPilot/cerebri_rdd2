#ifndef RDD2_EFMI_WRAPPER_H_
#define RDD2_EFMI_WRAPPER_H_

#define RDD2_EFMI_CAT_(a, b) a##b
#define RDD2_EFMI_CAT(a, b) RDD2_EFMI_CAT_(a, b)
#define RDD2_EFMI_CAT3_(a, b, c) a##b##c
#define RDD2_EFMI_CAT3(a, b, c) RDD2_EFMI_CAT3_(a, b, c)

#define RDD2_EFMI_STATE_TYPE(model) RDD2_EFMI_CAT(model, State)
#define RDD2_EFMI_STATE(model, name) RDD2_EFMI_STATE_TYPE(model) name
#define RDD2_EFMI_METHOD(model, method) RDD2_EFMI_CAT3(model, _, method)

#define RDD2_EFMI_INIT(model, state_ptr)                                                        \
	do {                                                                                     \
		*(state_ptr) = (RDD2_EFMI_STATE_TYPE(model)){0};                                 \
		RDD2_EFMI_METHOD(model, startup)(state_ptr);                                     \
		RDD2_EFMI_METHOD(model, recalibrate)(state_ptr);                                 \
	} while (0)

#define RDD2_EFMI_RECALIBRATE(model, state_ptr) RDD2_EFMI_METHOD(model, recalibrate)(state_ptr)
#define RDD2_EFMI_STEP(model, state_ptr) RDD2_EFMI_METHOD(model, dostep)(state_ptr)

#endif
