/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */
#include "tuple_constraint.h"

#include "trivia/data_bank.h"
#include "trivia/util.h"

int
tuple_constraint_noop_check(const struct tuple_constraint *constr,
			    const char *mp_data, const char *mp_data_end,
			    const struct tuple_field *field)
{
	(void)constr;
	(void)mp_data;
	(void)mp_data_end;
	(void)field;
	return 0;
}

void
tuple_constraint_noop_destructor(struct tuple_constraint *constr)
{
	(void)constr;
}

int
tuple_constraint_cmp(const struct tuple_constraint *constr1,
		     const struct tuple_constraint *constr2,
		     bool ignore_name)
{
	return tuple_constraint_def_cmp(&constr1->def, &constr2->def,
					ignore_name);
}

uint32_t
tuple_constraint_hash_process(const struct tuple_constraint *constr,
			      uint32_t *ph, uint32_t *pcarry)
{
	return tuple_constraint_def_hash_process(&constr->def, ph, pcarry);
}

struct tuple_constraint *
tuple_constraint_collocate(const struct tuple_constraint_def *defs,
			   size_t count)
{
	/** Data bank for structs tuple_constraint_fkey_data. */
	struct data_bank bank = data_bank_initializer();
	for (size_t i = 0; i < count; i++) {
		if (defs[i].type != CONSTR_FKEY)
			continue;
		size_t size = offsetof(struct tuple_constraint_fkey_data,
				       data[1]);
		data_bank_reserve_data(&bank, size);
	}
	struct tuple_constraint *res =
		tuple_constraint_def_collocate_raw(defs, count, sizeof(*res),
						   data_bank_size(&bank));
	/* Initialize uninitialized part. */
	data_bank_use(&bank, res + count);
	for (size_t i = 0; i < count; i++) {
		res[i].check = tuple_constraint_noop_check;
		res[i].destroy = tuple_constraint_noop_destructor;
		if (defs[i].type != CONSTR_FKEY) {
			res[i].fkey = NULL;
			continue;
		}
		size_t size = offsetof(struct tuple_constraint_fkey_data,
				       data[1]);
		res[i].fkey = data_bank_create_data(&bank, size);
		res[i].fkey->field_count = 1;
	}

	assert(data_bank_size(&bank) == 0);
	return res;
}
