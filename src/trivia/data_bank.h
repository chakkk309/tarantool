/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Data bank is a data structure that is designed for simplification of
 * allocation of several objects in one memory block. It could be anything,
 * but special attention is given to string objects, that are arrays of chars.
 * Each string is allocated with a personal null termination symbol, in the
 * end memory block while all other objects will be placed in the beginning
 * of the block. This gives some sort of guarantee object alignment: if all
 * objects have the same alignment as well as memory block - than all objects
 * will have correct (in terms of alignment) addresses.
 * Typical usage consist of two phases: gathering total needed size of memory
 * block and creation of strings in given block.
 */
struct data_bank {
	char *data;
	char *data_end;
};

/**
 * Default data_bank initializer. Just assign it to new data_bank and it's
 * ready for the first phase.
 */
static inline struct data_bank
data_bank_initializer()
{
	struct data_bank res = {NULL, NULL};
	return res;
}

/**
 * Phase 1: account am arbitrary data of @a size that is needed to be allocated.
 */
static inline void
data_bank_reserve_data(struct data_bank *bank, size_t size)
{
	bank->data_end += size;
}

/**
 * Phase 1: account a string of @a size that is needed to be allocated,
 * including char for null-termination.
 */
static inline void
data_bank_reserve_str(struct data_bank *bank, size_t size)
{
	bank->data_end += size + 1;
}

/**
 * Phase 1 end: get total memory size required for all data.
 */
static inline size_t
data_bank_size(struct data_bank *bank)
{
	return bank->data_end - bank->data;
}

/**
 * Phase 2 begin: provide a block of memory of required size.
 */
static inline void
data_bank_use(struct data_bank *bank, void *data)
{
	bank->data_end = (char *) data + (bank->data_end - bank->data);
	bank->data = data;
}

/**
 * Phase 2: allocate and return an arbitrary data block with given @a size.
 */
static inline void *
data_bank_create_data(struct data_bank *bank, size_t size)
{
	char *res = bank->data;
	bank->data += size;
	return res;
}

/**
 * Phase 2: allocate and fill a string with given data @a src of given size
 * @a src_size, including null-termination. Return new string.
 */
static inline char *
data_bank_create_str(struct data_bank *bank, const char *src, size_t src_size)
{
	bank->data_end--;
	*bank->data_end = 0;
	bank->data_end -= src_size;
	memcpy(bank->data_end, src, src_size);
	return bank->data_end;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
