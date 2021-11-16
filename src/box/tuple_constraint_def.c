/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */
#include "tuple_constraint_def.h"

#include "trivia/util.h"
#include "trivia/data_bank.h"
#include "small/region.h"
#include "msgpuck.h"
#include "PMurHash.h"

int
tuple_constraint_def_cmp_func(const struct tuple_constraint_func_def *def1,
			      const struct tuple_constraint_func_def *def2)
{
	return def1->id < def2->id ? -1 : def1->id > def2->id;
}

int
tuple_constraint_def_cmp(const struct tuple_constraint_def *def1,
			 const struct tuple_constraint_def *def2,
			 bool ignore_name)
{
	int rc;
	if (!ignore_name) {
		if (def1->name_len != def2->name_len)
			return def1->name_len < def2->name_len ? -1 : 1;
		if ((rc = memcmp(def1->name, def2->name, def1->name_len)) != 0)
			return rc;
	}
	return tuple_constraint_def_cmp_func(&def1->func, &def2->func);
}

uint32_t
tuple_constraint_def_hash_process(const struct tuple_constraint_def *def,
				  uint32_t *ph, uint32_t *pcarry)
{
	PMurHash32_Process(ph, pcarry, def->name, def->name_len);
	PMurHash32_Process(ph, pcarry, &def->func.id, sizeof(def->func.id));
	return def->name_len + sizeof(def->func.id);
}

int
tuple_constraint_def_decode(const char **data,
			    struct tuple_constraint_def **def, uint32_t *count,
			    struct region *region, const char **error)
{
	/* Expected normal form of constraints: {name1=func1, name2=func2..}. */
	if (mp_typeof(**data) != MP_MAP) {
		*error = "constraint field is expected to be a MAP";
		return -1;
	}

	uint32_t map_size = mp_decode_map(data);
	*count = map_size;
	if (*count == 0)
		return 0;

	int bytes;
	*def = region_alloc_array(region, struct tuple_constraint_def,
				  *count, &bytes);
	if (*def == NULL) {
		*error = "array of constraints";
		return bytes;
	}
	struct tuple_constraint_def *new_def = *def;

	for (size_t i = 0; i < map_size; i++) {
		if (mp_typeof(**data) != MP_STR) {
			*error = "constraint name is expected to be a string";
			return -1;
		}
		uint32_t str_len;
		const char *str = mp_decode_str(data, &str_len);
		char *str_copy = region_alloc(region, str_len + 1);
		if (str_copy == NULL) {
			*error = i % 2 == 0 ? "constraint name"
					    : "constraint func";
			return str_len + 1;
		}
		memcpy(str_copy, str, str_len);
		str_copy[str_len] = 0;
		new_def[i].name = str_copy;
		new_def[i].name_len = str_len;

		if (mp_typeof(**data) != MP_UINT) {
			*error = "constraint function ID "
				 "is expected to be a number";
			return -1;
		}
		new_def[i].func.id = mp_decode_uint(data);
	}
	return 0;
}

/**
 * Reserve strings needed for given constraint definition @a dev in given
 * string @a bank.
 */
static void
tuple_constraint_def_reserve_bank(const struct tuple_constraint_def *def,
				  struct data_bank *bank)
{
	data_bank_reserve_str(bank, def->name_len);
}

/**
 * Copy constraint definition from @a str to @ dst, allocating strings on
 * string @a bank.
 */
static void
tuple_constraint_def_copy(struct tuple_constraint_def *dst,
			  const struct tuple_constraint_def *src,
			  struct data_bank *bank)
{
	dst->name = data_bank_create_str(bank, src->name, src->name_len);
	dst->name_len = src->name_len;
	dst->func.id = src->func.id;
}

struct tuple_constraint_def *
tuple_constraint_def_collocate(const struct tuple_constraint_def *defs,
			       size_t count)
{
	struct tuple_constraint_def *res =
		tuple_constraint_def_collocate_raw(defs, count, sizeof(*res), 0);
	return res;
}

void *
tuple_constraint_def_collocate_raw(const struct tuple_constraint_def *defs,
				   size_t count, size_t object_size,
				   size_t additional_size)
{
	if (count == 0)
		return NULL;

	/*
	 * Check that object is a multiple of 8 and round up additional_size to
	 * the closest multiple of 8, to make sure that data bank will be
	 * aligned properly. It's not necessary for string, but in general
	 * data_back can also allocate objects that can require alignment.
	 * If object_size and additional_size will be multiple of 8 we would
	 * guarantee that objects will be aligned to 8 bytes.
	 */
	assert(object_size % 8 == 0);
	additional_size = (additional_size + 7) & ~7;

	struct data_bank bank = data_bank_initializer();
	/* Calculate needed space. */
	for (size_t i = 0; i < count; i++)
		tuple_constraint_def_reserve_bank(&defs[i], &bank);
	size_t total_size =
		object_size * count + additional_size + data_bank_size(&bank);

	/* Allocate block. */
	char *res = xmalloc(total_size);
	data_bank_use(&bank, res + object_size * count + additional_size);

	/* Now constraint defs in the new array. */
	for (size_t i = 0; i < count; i++) {
		struct tuple_constraint_def *def =
			(struct tuple_constraint_def *)(res + i * object_size);
		tuple_constraint_def_copy(def, &defs[i], &bank);
	}

	/* If we did it correctly then there is no more space for strings. */
	assert(data_bank_size(&bank) == 0);
	return res;
}
