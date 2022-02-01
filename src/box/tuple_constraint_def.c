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

static int
id_or_name_def_cmp(const struct tuple_constraint_id_or_name_def *def1,
		   const struct tuple_constraint_id_or_name_def *def2)
{
	if (def1->id != def2->id)
		return def1->id < def2->id ? -1 : 1;
	if (def1->name_len != def2->name_len)
		return def1->name_len < def2->name_len ? -1 : 1;
	return memcmp(def1->name, def2->name, def1->name_len);
}

static int
tuple_constraint_def_cmp_fkey(const struct tuple_constraint_fkey_def *def1,
			      const struct tuple_constraint_fkey_def *def2)
{
	if (def1->space_id != def2->space_id)
		return def1->space_id < def2->space_id ? -1 : 1;
	if (def1->field_mapping_size != def2->field_mapping_size)
		return def1->field_mapping_size < def2->field_mapping_size ? -1 : 1;
	if (def1->field_mapping_size == 0)
		return id_or_name_def_cmp(&def1->field, &def2->field);

	for (uint32_t i = 0; i < def1->field_mapping_size; i++) {
		int rc;
		rc = id_or_name_def_cmp(&def1->field_mapping[i].local_field,
					&def2->field_mapping[i].local_field);
		if (rc != 0)
			return rc;
		rc = id_or_name_def_cmp(&def1->field_mapping[i].foreign_field,
					&def2->field_mapping[i].foreign_field);
		if (rc != 0)
			return rc;
	}
	return 0;
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
	if (def1->type != def2->type)
		return def1->type < def2->type ? -1 : 1;
	if (def1->type == CONSTR_FUNC) {
		return tuple_constraint_def_cmp_func(&def1->func, &def2->func);
	} else {
		assert(def1->type == CONSTR_FKEY);
		return tuple_constraint_def_cmp_fkey(&def1->fkey, &def2->fkey);
	}
}

static uint32_t
id_or_name_def_hash_process(const struct tuple_constraint_id_or_name_def *def,
			    uint32_t *ph, uint32_t *pcarry)
{
	PMurHash32_Process(ph, pcarry, &def->id, sizeof(def->id));
	PMurHash32_Process(ph, pcarry, &def->name_len, sizeof(def->name_len));
	PMurHash32_Process(ph, pcarry, &def->name, def->name_len);
	return sizeof(def->id) + sizeof(def->name_len) + def->name_len;
}

static uint32_t
field_mapping_hash_process(const struct tuple_constraint_fkey_def *def,
			   uint32_t *ph, uint32_t *pcarry)
{
	uint32_t bytes = 0;
	for (uint32_t i = 0; i < def->field_mapping_size; i++) {
		struct tuple_constraint_fkey_field_mapping *m =
			&def->field_mapping[i];
		bytes += id_or_name_def_hash_process(&m->local_field,
						     ph, pcarry);
		bytes += id_or_name_def_hash_process(&m->foreign_field,
						     ph, pcarry);
	}
	return bytes;
}

uint32_t
tuple_constraint_def_hash_process(const struct tuple_constraint_def *def,
				  uint32_t *ph, uint32_t *pcarry)
{
	PMurHash32_Process(ph, pcarry, def->name, def->name_len);
	PMurHash32_Process(ph, pcarry, &def->type, sizeof(def->type));
	uint32_t bytes = def->name_len + sizeof(def->type);
	if (def->type == CONSTR_FUNC) {
		PMurHash32_Process(ph, pcarry,
				   &def->func.id, sizeof(def->func.id));
		bytes += sizeof(def->func.id);
	} else {
		assert(def->type == CONSTR_FKEY);
		PMurHash32_Process(ph, pcarry, &def->fkey.space_id,
				   sizeof(def->fkey.space_id));
		bytes += sizeof(def->fkey.space_id);
		PMurHash32_Process(ph, pcarry, &def->fkey.field_mapping_size,
				   sizeof(def->fkey.field_mapping_size));
		bytes += sizeof(def->fkey.field_mapping_size);
		if (def->fkey.field_mapping_size == 0)
			bytes += id_or_name_def_hash_process(&def->fkey.field,
							     ph, pcarry);
		else
			bytes += field_mapping_hash_process(&def->fkey,
							     ph, pcarry);
	}
	return bytes;
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

	uint32_t old_count = *count;
	struct tuple_constraint_def *old_def = *def;
	uint32_t map_size = mp_decode_map(data);
	*count += map_size;
	if (*count == 0)
		return 0;

	int bytes;
	*def = region_alloc_array(region, struct tuple_constraint_def,
				  *count, &bytes);
	if (*def == NULL) {
		*error = "array of constraints";
		return bytes;
	}
	for (uint32_t i = 0; i < old_count; i++)
		(*def)[i] = old_def[i];
	struct tuple_constraint_def *new_def = *def + old_count;

	for (size_t i = 0; i < map_size; i++)
		new_def[i].type = CONSTR_FUNC;

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

static int
space_id_decode(const char **data, uint32_t *space_id, const char **error)
{
	if (mp_typeof(**data) != MP_UINT) {
		*error = "foreign key: space must be a number";
		return -1;
	}
	*space_id = mp_decode_uint(data);
	return 0;
}

static int
id_or_name_def_decode(const char **data,
		      struct tuple_constraint_id_or_name_def *def,
		      struct region *region, const char **error)
{
	if (mp_typeof(**data) == MP_UINT) {
		def->id = mp_decode_uint(data);
		def->name = "";
		def->name_len = 0;
	} else if (mp_typeof(**data) == MP_STR) {
		const char *str = mp_decode_str(data, &def->name_len);
		char *str_copy = region_alloc(region, def->name_len + 1);
		if (str_copy == NULL) {
			*error = "string";
			return def->name_len + 1;
		}
		memcpy(str_copy, str, def->name_len);
		str_copy[def->name_len] = 0;
		def->name = str_copy;
		def->id = 0;
	} else {
		*error = "foreign key: field must be number or string";
		return -1;
	}
	return 0;
}

/**
 * Helper function of tuple_constraint_def_decode_fkey.
 * Decode foreign key field mapping, that is expected to be MP_MAP with
 * local field (id or name) -> foreign field (id or name) correspondence.
 */
static int
field_mapping_decode(const char **data,
		     struct tuple_constraint_fkey_def *fkey,
		     struct region *region, const char **error)
{
	if (mp_typeof(**data) != MP_MAP) {
		*error = "field mapping is expected to be a map";
		return -1;
	}
	uint32_t mapping_size = mp_decode_map(data);
	if (mapping_size == 0) {
		*error = "field mapping is expected to be a map";
		return -1;
	}
	fkey->field_mapping_size = mapping_size;
	size_t sz;
	fkey->field_mapping = region_alloc_array(region,
		struct tuple_constraint_fkey_field_mapping, mapping_size, &sz);
	if (fkey->field_mapping == NULL) {
		*error = "field mapping";
		return sz;
	}
	for (uint32_t i = 0 ; i < 2 * mapping_size; i++) {
		struct tuple_constraint_id_or_name_def *def = i % 2 == 0 ?
			&fkey->field_mapping[i / 2].local_field :
			&fkey->field_mapping[i / 2].foreign_field;
		int rc = id_or_name_def_decode(data, def, region, error);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int
tuple_constraint_def_decode_fkey(const char **data,
				 struct tuple_constraint_def **def,
				 uint32_t *count, struct region *region,
				 const char **error, bool is_complex)
{
	/*
	 * Expected normal form of foreign keys: {name1=data1, name2=data2..},
	 * where dataX has form: {space=id/name, field=id/name}
	 */
	if (mp_typeof(**data) != MP_MAP) {
		*error = "foreign key field is expected to be a MAP";
		return -1;
	}

	uint32_t old_count = *count;
	struct tuple_constraint_def *old_def = *def;
	uint32_t map_size = mp_decode_map(data);
	*count += map_size;
	if (*count == 0)
		return 0;

	int bytes;
	*def = region_alloc_array(region, struct tuple_constraint_def,
				  *count, &bytes);
	if (*def == NULL) {
		*error = "array of constraints";
		return bytes;
	}
	for (uint32_t i = 0; i < old_count; i++)
		(*def)[i] = old_def[i];
	struct tuple_constraint_def *new_def = *def + old_count;

	for (size_t i = 0; i < *count; i++) {
		if (mp_typeof(**data) != MP_STR) {
			*error = "foreign key name is expected to be a string";
			return -1;
		}
		uint32_t str_len;
		const char *str = mp_decode_str(data, &str_len);
		char *str_copy = region_alloc(region, str_len + 1);
		if (str_copy == NULL) {
			*error = "constraint name";
			return str_len + 1;
		}
		memcpy(str_copy, str, str_len);
		str_copy[str_len] = 0;
		new_def[i].name = str_copy;
		new_def[i].name_len = str_len;
		new_def[i].type = CONSTR_FKEY;
		if (mp_typeof(**data) != MP_MAP) {
			*error = "foreign key definition "
				 "is expected to be a map";
			return -1;
		}
		new_def[i].fkey.field_mapping_size = 0;
		uint32_t def_size = mp_decode_map(data);
		bool has_space = false, has_field = false;
		for (size_t j = 0; j < def_size; j++) {
			if (mp_typeof(**data) != MP_STR) {
				*error = "foreign key definition key "
					 "is expected to be a string";
				return -1;
			}
			uint32_t key_len;
			const char *key = mp_decode_str(data, &key_len);
			bool is_space;
			if (key_len == strlen("space") &&
			    memcmp(key, "space", key_len) == 0) {
				is_space = true;
				has_space = true;
			} else if (key_len == strlen("field") &&
				memcmp(key, "field", key_len) == 0) {
				is_space = false;
				has_field = true;
			} else {
				*error = "foreign key definition is expected "
					 "to be {space=.., field=..}";
				return -1;
			}
			int rc;
			struct tuple_constraint_fkey_def *fk = &new_def[i].fkey;
			if (is_space)
				rc = space_id_decode(data, &fk->space_id,
						     error);
			else if (!is_complex)
				rc = id_or_name_def_decode(data, &fk->field,
							   region, error);
			else
				rc = field_mapping_decode(data, fk,
							  region, error);
			if (rc != 0)
				return rc;
		}
		if (!has_space || !has_field) {
			*error = "foreign key definition is expected "
				 "to be {space=.., field=..}";
			return -1;
		}
	}
	return 0;
}

static void
id_or_name_def_reserve_bank(const struct tuple_constraint_id_or_name_def *def,
			    struct data_bank *bank)
{
	/* Reservation is required only for strings. */
	if (def->name_len != 0)
		data_bank_reserve_str(bank, def->name_len);
}

static void
id_or_name_def_copy(struct tuple_constraint_id_or_name_def *dst,
		    const struct tuple_constraint_id_or_name_def *src,
		    struct data_bank *bank)
{
	dst->id = src->id;
	dst->name_len = src->name_len;
	if (src->name_len != 0)
		dst->name = data_bank_create_str(bank, src->name, src->name_len);
	else
		dst->name = "";
}

static void
field_mapping_reserve_bank(const struct tuple_constraint_fkey_def *def,
			   struct data_bank *bank)
{
	assert(def->field_mapping_size != 0);
	size_t bytes = def->field_mapping_size * sizeof(def->field_mapping[0]);
	data_bank_reserve_data(bank, bytes);
	for (uint32_t i = 0; i < def->field_mapping_size; i++) {
		const struct tuple_constraint_fkey_field_mapping *f =
			&def->field_mapping[i];
		id_or_name_def_reserve_bank(&f->local_field, bank);
		id_or_name_def_reserve_bank(&f->foreign_field, bank);
	}
}

static void
field_mapping_copy(struct tuple_constraint_fkey_def *dst,
		   const struct tuple_constraint_fkey_def *src,
		   struct data_bank *bank)
{
	assert(src->field_mapping_size != 0);
	dst->field_mapping_size = src->field_mapping_size;
	size_t bytes = src->field_mapping_size * sizeof(dst->field_mapping[0]);
	dst->field_mapping = data_bank_create_data(bank, bytes);
	for (uint32_t i = 0; i < src->field_mapping_size; i++) {
		struct tuple_constraint_fkey_field_mapping *d =
			&dst->field_mapping[i];
		const struct tuple_constraint_fkey_field_mapping *s =
			&src->field_mapping[i];
		id_or_name_def_copy(&d->local_field, &s->local_field, bank);
		id_or_name_def_copy(&d->foreign_field, &s->foreign_field, bank);
	}
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
	if (def->type == CONSTR_FUNC) {
	} else {
		assert(def->type == CONSTR_FKEY);
		if (def->fkey.field_mapping_size == 0)
			id_or_name_def_reserve_bank(&def->fkey.field, bank);
		else
			field_mapping_reserve_bank(&def->fkey, bank);
	}
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
	dst->type = src->type;
	if (src->type == CONSTR_FUNC) {
		dst->func.id = src->func.id;
	} else {
		assert(src->type == CONSTR_FKEY);
		dst->fkey.space_id = src->fkey.space_id;
		dst->fkey.field_mapping_size = 0;
		if (src->fkey.field_mapping_size == 0)
			id_or_name_def_copy(&dst->fkey.field, &src->fkey.field,
					    bank);
		else
			field_mapping_copy(&dst->fkey, &src->fkey, bank);
	}
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
