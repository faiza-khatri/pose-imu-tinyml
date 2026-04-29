/*
 * Copyright 2020 The TensorFlow Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Copyright (c) 2025 Aerlync Labs Inc.
 */

#include "main_functions.h"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <zephyr/kernel.h>

#include "constants.h"
// #include "dense_quantized_model.h"
#ifdef CONFIG_USE_CNN_MODEL
    #include "../models/cnn/cnn_quantized_model.h"
#else
    #include "../models/dense/dense_quantized_model.h"
#endif

#include "output_handler.hpp"
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>


/* Globals, used for compatibility with Arduino-style sketches. */
namespace {
	const tflite::Model *model = nullptr;
	int inference_count = 0;
	// constexpr int kTensorArenaSize = 60 * 1024; // 60KB

	#ifdef CONFIG_USE_CNN_MODEL
		constexpr int kTensorArenaSize = 100 * 1024;
	#else
		constexpr int kTensorArenaSize = 60 * 1024;
	#endif
	// constexpr int kTensorArenaSize = 2000;
	uint8_t tensor_arena[kTensorArenaSize];
}  /* namespace */

TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
extern uint32_t t_start_full;

/* The name of this function is important for Arduino compatibility. */
void setup(void)
{
	/* Map the model into a usable data structure. This doesn't involve any
	 * copying or parsing, it's a very lightweight operation.
	 */
	t_start_full = k_cycle_get_32(); //start end-to-end timer

	MicroPrintf("setup: starting");
	// model = tflite::GetModel(dense_quantized_model);
	#ifdef CONFIG_USE_CNN_MODEL
		model = tflite::GetModel(cnn_quantized_model);
	#else
		model = tflite::GetModel(dense_quantized_model);
	#endif

	    MicroPrintf("setup: model loaded");


	if (model->version() != TFLITE_SCHEMA_VERSION) {
		MicroPrintf("Model provided is schema version %d not equal "
					"to supported version %d.",
					model->version(), TFLITE_SCHEMA_VERSION);
		return;
	}

	    MicroPrintf("setup: schema ok");
	/* This pulls in the operation implementations we need.
	 * NOLINTNEXTLINE(runtime-global-variables)
	 */
	// static tflite::MicroMutableOpResolver <3> resolver;
	// resolver.AddFullyConnected();
	// resolver.AddReshape();
	// resolver.AddSoftmax();
	// // resolver.AddTanh();                                  // add this

	#ifdef CONFIG_USE_CNN_MODEL
		static tflite::MicroMutableOpResolver<5> resolver;
		resolver.AddConv2D();
		resolver.AddMaxPool2D();
		resolver.AddFullyConnected();
		resolver.AddReshape();
		resolver.AddSoftmax();
	#else
		static tflite::MicroMutableOpResolver<3> resolver;
		resolver.AddFullyConnected();
		resolver.AddReshape();
		resolver.AddSoftmax();
	#endif

	    MicroPrintf("setup: resolver ok");

	/* Build an interpreter to run the model with. */
	static tflite::MicroInterpreter static_interpreter(
		model, resolver, tensor_arena, kTensorArenaSize);
	interpreter = &static_interpreter;

	    MicroPrintf("setup: interpreter ok");

	/* Allocate memory from the tensor_arena for the model's tensors. */
	TfLiteStatus allocate_status = interpreter->AllocateTensors();
	if (allocate_status != kTfLiteOk) {
		MicroPrintf("AllocateTensors() failed");
		return;
	}

	MicroPrintf("setup: tensors allocated, arena used: %d",
                interpreter->arena_used_bytes());

	/* Obtain pointers to the model's input and output tensors. */
	input = interpreter->input(0);
	output = interpreter->output(0);

	/* Keep track of how many inferences we have performed. */
	inference_count = 0;

	    MicroPrintf("setup: complete");
}

