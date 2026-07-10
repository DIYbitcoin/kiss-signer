#include "py/obj.h"
#include "py/objexcept.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "src/types/bip39.h"
#include "src/types/bytes_type.h"
#include "src/types/output.h"
#include "src/types/psbt.h"
#include "src/ur.h"
#include "src/ur_decoder.h"
#include "src/ur_encoder.h"

// URDecoder class structure
typedef struct {
  mp_obj_base_t base;
  ur_decoder_t *decoder;
} mp_obj_ur_decoder_t;

// UREncoder class structure
typedef struct {
  mp_obj_base_t base;
  ur_encoder_t *encoder;
  mp_obj_t fountain_encoder_cached; // Cached fountain_encoder wrapper to
                                    // prevent memory leaks
} mp_obj_ur_encoder_t;

// Forward declarations
static const mp_obj_type_t mp_type_ur_decoder;
static const mp_obj_type_t mp_type_ur_encoder;

// Shared UR decoder error -> MicroPython exception mapping. Called on
// any path that produces an error; mp_raise_msg does not return so the
// helper is effectively NORETURN.
static void raise_ur_decoder_error(ur_decoder_error_t error) {
  switch (error) {
  case UR_DECODER_ERROR_INVALID_SCHEME:
    mp_raise_msg(&mp_type_ValueError, "Invalid UR scheme");
    break;
  case UR_DECODER_ERROR_INVALID_TYPE:
    mp_raise_msg(&mp_type_ValueError, "Invalid UR type");
    break;
  case UR_DECODER_ERROR_INVALID_PATH_LENGTH:
    mp_raise_msg(&mp_type_ValueError, "Invalid UR path length");
    break;
  case UR_DECODER_ERROR_INVALID_SEQUENCE_COMPONENT:
    mp_raise_msg(&mp_type_ValueError, "Invalid sequence component");
    break;
  case UR_DECODER_ERROR_INVALID_FRAGMENT:
    mp_raise_msg(&mp_type_ValueError, "Invalid fragment");
    break;
  case UR_DECODER_ERROR_INVALID_PART:
    mp_raise_msg(&mp_type_ValueError, "Invalid part");
    break;
  case UR_DECODER_ERROR_INVALID_CHECKSUM:
    mp_raise_msg(&mp_type_ValueError, "Invalid checksum");
    break;
  case UR_DECODER_ERROR_MEMORY:
    mp_raise_msg(&mp_type_RuntimeError, "Memory error");
    break;
  default:
    mp_raise_msg(&mp_type_RuntimeError, "URDecoder error");
    break;
  }
}

// UR class structure for returning results (matches Python UR interface)
typedef struct {
  mp_obj_base_t base;
  ur_t *ur;
} mp_obj_ur_t;

static const mp_obj_type_t mp_type_ur;

// UR implementation (matches Python UR class)
static void ur_print(const mp_print_t *print, mp_obj_t self_in,
                     mp_print_kind_t kind) {
  (void)kind;
  mp_obj_ur_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->ur) {
    mp_printf(print, "UR(type='%s')", ur_get_type(self->ur));
  } else {
    mp_printf(print, "UR(invalid)");
  }
}

static mp_obj_t ur_make_new(const mp_obj_type_t *type, size_t n_args,
                            size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 2, 2, false);

  // Extract type and CBOR data from args
  const char *ur_type = mp_obj_str_get_str(args[0]);

  mp_buffer_info_t cbor_buf;
  mp_get_buffer_raise(args[1], &cbor_buf, MP_BUFFER_READ);

  // Create internal UR object first so a failure doesn't leak the wrapper
  // through the exception long-jump in mp_raise_msg.
  ur_t *ur = ur_new(ur_type, cbor_buf.buf, cbor_buf.len);
  if (!ur) {
    mp_raise_msg(&mp_type_MemoryError, "Failed to create UR object");
  }

  // with_finaliser so __del__ (ur_del) runs on GC and frees ur_t.
  mp_obj_ur_t *self = m_new_obj_with_finaliser(mp_obj_ur_t);
  self->base.type = type;
  self->ur = ur;

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ur_del(mp_obj_t self_in) {
  mp_obj_ur_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->ur) {
    ur_free(self->ur);
    self->ur = NULL;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_del_obj, ur_del);

// UR attributes
static void ur_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  mp_obj_ur_t *self = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL) {
    // Load attribute
    if (attr == MP_QSTR_type) {
      if (self->ur) {
        dest[0] = mp_obj_new_str(ur_get_type(self->ur),
                                 strlen(ur_get_type(self->ur)));
      } else {
        dest[0] = mp_const_none;
      }
    } else if (attr == MP_QSTR_cbor) {
      if (self->ur) {
        dest[0] = mp_obj_new_bytearray(ur_get_cbor_len(self->ur),
                                       ur_get_cbor(self->ur));
      } else {
        dest[0] = mp_const_none;
      }
    }
  }
}

// UR locals dict
static const mp_rom_map_elem_t ur_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ur_del_obj)},
};
static MP_DEFINE_CONST_DICT(ur_locals_dict, ur_locals_dict_table);

static const mp_obj_type_t mp_type_ur = {
    {&mp_type_type},   .name = MP_QSTR_UR,
    .print = ur_print, .make_new = ur_make_new,
    .attr = ur_attr,   .locals_dict = (mp_obj_dict_t *)&ur_locals_dict,
};

// URDecoder implementation
static void ur_decoder_print(const mp_print_t *print, mp_obj_t self_in,
                             mp_print_kind_t kind) {
  (void)kind;
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);
  double progress = ur_decoder_estimated_percent_complete(self->decoder);
  mp_printf(print, "URDecoder(complete=%s, progress=%.1f%%)",
            ur_decoder_is_complete(self->decoder) ? "True" : "False",
            progress * 100.0);
}

static mp_obj_t ur_decoder_make_new(const mp_obj_type_t *type, size_t n_args,
                                    size_t n_kw, const mp_obj_t *args) {
  mp_arg_check_num(n_args, n_kw, 0, 0, false);

  // Allocate the C decoder before the wrapper so a failure doesn't leak the
  // wrapper through mp_raise_msg's long-jump.
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    mp_raise_msg(&mp_type_MemoryError, "Failed to create URDecoder");
  }

  // with_finaliser so __del__ (ur_decoder_del) runs on GC and frees decoder.
  mp_obj_ur_decoder_t *self = m_new_obj_with_finaliser(mp_obj_ur_decoder_t);
  self->base.type = type;
  self->decoder = decoder;

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ur_decoder_del(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->decoder) {
    ur_decoder_free(self->decoder);
    self->decoder = NULL;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_del_obj, ur_decoder_del);

// receive_part method
static mp_obj_t ur_decoder_receive_part_py(mp_obj_t self_in,
                                           mp_obj_t part_str) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    mp_raise_msg(&mp_type_RuntimeError, "URDecoder is closed");
  }

  const char *part_cstr = mp_obj_str_get_str(part_str);
  bool result = ur_decoder_receive_part(self->decoder, part_cstr);

  if (!result) {
    raise_ur_decoder_error(ur_decoder_get_last_error(self->decoder));
  }

  return mp_obj_new_bool(result);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ur_decoder_receive_part_obj,
                                 ur_decoder_receive_part_py);

// is_complete method
static mp_obj_t ur_decoder_is_complete_py(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    return mp_const_false;
  }

  return mp_obj_new_bool(ur_decoder_is_complete(self->decoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_is_complete_obj,
                                 ur_decoder_is_complete_py);

// is_success method
static mp_obj_t ur_decoder_is_success_py(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    return mp_const_false;
  }

  return mp_obj_new_bool(ur_decoder_is_success(self->decoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_is_success_obj,
                                 ur_decoder_is_success_py);

// result property
static mp_obj_t ur_decoder_result(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    mp_raise_msg(&mp_type_RuntimeError, "URDecoder is closed");
  }

  // Check if decoding was successful before getting the result
  if (!ur_decoder_is_success(self->decoder)) {
    raise_ur_decoder_error(ur_decoder_get_last_error(self->decoder));
  }

  ur_result_t *result = ur_decoder_get_result(self->decoder);
  if (!result) {
    return mp_const_none;
  }

  // Create UR object (matches Python interface)
  mp_obj_t type_str = mp_obj_new_str(result->type, strlen(result->type));
  mp_obj_t cbor_bytes =
      mp_obj_new_bytearray(result->cbor_len, result->cbor_data);

  mp_obj_t args[2] = {type_str, cbor_bytes};
  return ur_make_new(&mp_type_ur, 2, 0, args);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_result_obj, ur_decoder_result);

// expected_part_count method
static mp_obj_t ur_decoder_expected_part_count_py(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    return mp_obj_new_int(0);
  }

  return mp_obj_new_int(ur_decoder_expected_part_count(self->decoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_expected_part_count_obj,
                                 ur_decoder_expected_part_count_py);

// processed_parts_count property
static mp_obj_t ur_decoder_processed_parts_count_py(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    return mp_obj_new_int(0);
  }

  return mp_obj_new_int(ur_decoder_processed_parts_count(self->decoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_processed_parts_count_obj,
                                 ur_decoder_processed_parts_count_py);

// estimated_percent_complete method
static mp_obj_t ur_decoder_estimated_percent_complete_py(mp_obj_t self_in) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->decoder) {
    return mp_obj_new_float(0.0);
  }

  return mp_obj_new_float(ur_decoder_estimated_percent_complete(self->decoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_decoder_estimated_percent_complete_obj,
                                 ur_decoder_estimated_percent_complete_py);

// URDecoder locals dict
static const mp_rom_map_elem_t ur_decoder_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ur_decoder_del_obj)},
    {MP_ROM_QSTR(MP_QSTR_receive_part),
     MP_ROM_PTR(&ur_decoder_receive_part_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_complete), MP_ROM_PTR(&ur_decoder_is_complete_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_success), MP_ROM_PTR(&ur_decoder_is_success_obj)},
    {MP_ROM_QSTR(MP_QSTR_estimated_percent_complete),
     MP_ROM_PTR(&ur_decoder_estimated_percent_complete_obj)},
};
static MP_DEFINE_CONST_DICT(ur_decoder_locals_dict,
                            ur_decoder_locals_dict_table);

// URDecoder attributes
static void ur_decoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  mp_obj_ur_decoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL) {
    // Load attribute
    if (attr == MP_QSTR_result) {
      if (!self->decoder) {
        dest[0] = mp_const_none;
        return;
      }

      ur_result_t *result = ur_decoder_get_result(self->decoder);
      if (!result) {
        dest[0] = mp_const_none;
        return;
      }

      // Create UR object
      mp_obj_t type_str = mp_obj_new_str(result->type, strlen(result->type));
      mp_obj_t cbor_bytes =
          mp_obj_new_bytearray(result->cbor_len, result->cbor_data);

      mp_obj_t args[2] = {type_str, cbor_bytes};
      dest[0] = ur_make_new(&mp_type_ur, 2, 0, args);
    } else if (attr == MP_QSTR_expected_part_count) {
      if (!self->decoder) {
        dest[0] = mp_obj_new_int(0);
      } else {
        dest[0] = mp_obj_new_int(ur_decoder_expected_part_count(self->decoder));
      }
    } else if (attr == MP_QSTR_processed_parts_count) {
      if (!self->decoder) {
        dest[0] = mp_obj_new_int(0);
      } else {
        dest[0] =
            mp_obj_new_int(ur_decoder_processed_parts_count(self->decoder));
      }
    } else {
      // Method lookup from locals_dict. Use the method-load protocol
      // (dest[0]=method, dest[1]=self) instead of allocating a bound
      // method — this path is hit by the GC finaliser looking up __del__,
      // and allocating during a sweep can trigger nlr_jump_fail.
      mp_obj_dict_t *locals_dict = (mp_obj_dict_t *)&ur_decoder_locals_dict;
      mp_map_elem_t *elem = mp_map_lookup(&locals_dict->map,
                                          MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
      if (elem != NULL) {
        dest[0] = elem->value;
        dest[1] = self_in;
      }
    }
  }
}

// URDecoder type definition
static const mp_obj_type_t mp_type_ur_decoder = {
    {&mp_type_type},
    .name = MP_QSTR_URDecoder,
    .print = ur_decoder_print,
    .make_new = ur_decoder_make_new,
    .attr = ur_decoder_attr,
    .locals_dict = (mp_obj_dict_t *)&ur_decoder_locals_dict,
};

// UREncoder implementation
static void ur_encoder_print(const mp_print_t *print, mp_obj_t self_in,
                             mp_print_kind_t kind) {
  (void)kind;
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->encoder) {
    mp_printf(print, "UREncoder(seq_len=%u, complete=%s)",
              ur_encoder_seq_len(self->encoder),
              ur_encoder_is_complete(self->encoder) ? "True" : "False");
  } else {
    mp_printf(print, "UREncoder(invalid)");
  }
}

static mp_obj_t ur_encoder_make_new(const mp_obj_type_t *type, size_t n_args,
                                    size_t n_kw, const mp_obj_t *args) {
  enum {
    ARG_ur,
    ARG_max_fragment_len,
    ARG_first_seq_num,
    ARG_min_fragment_len
  };
  static const mp_arg_t allowed_args[] = {
      {MP_QSTR_ur, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
      {MP_QSTR_max_fragment_len, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_first_seq_num, MP_ARG_INT, {.u_int = 0}},
      {MP_QSTR_min_fragment_len, MP_ARG_INT, {.u_int = 10}},
  };

  mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
  mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args),
                            allowed_args, parsed_args);

  // Type-check BEFORE MP_OBJ_TO_PTR — otherwise passing a non-UR object
  // dereferences garbage as ur_t*.
  if (!mp_obj_is_type(parsed_args[ARG_ur].u_obj, &mp_type_ur)) {
    mp_raise_TypeError("First argument must be a UR object");
  }
  mp_obj_ur_t *ur_obj = MP_OBJ_TO_PTR(parsed_args[ARG_ur].u_obj);

  if (!ur_obj->ur) {
    mp_raise_msg(&mp_type_ValueError, "Invalid UR object");
  }

  // Clamp fragment-length params coming from Python. 10..2048 covers any
  // real QR payload; values outside drive unbounded fountain allocations.
  mp_int_t raw_max = parsed_args[ARG_max_fragment_len].u_int;
  mp_int_t raw_min = parsed_args[ARG_min_fragment_len].u_int;
  if (raw_max < 10 || raw_max > 2048 || raw_min < 1 || raw_min > raw_max) {
    mp_raise_msg(&mp_type_ValueError,
                 "fragment_len out of range (min 1..max, max 10..2048)");
  }
  size_t max_fragment_len = (size_t)raw_max;
  size_t min_fragment_len = (size_t)raw_min;
  uint32_t first_seq_num = parsed_args[ARG_first_seq_num].u_int;

  // Allocate the C encoder before the wrapper so a failure doesn't leak the
  // wrapper through mp_raise_msg's long-jump.
  const char *ur_type_str = ur_get_type(ur_obj->ur);
  const uint8_t *cbor_data = ur_get_cbor(ur_obj->ur);
  size_t cbor_len = ur_get_cbor_len(ur_obj->ur);
  ur_encoder_t *encoder =
      ur_encoder_new(ur_type_str, cbor_data, cbor_len, max_fragment_len,
                     first_seq_num, min_fragment_len);
  if (!encoder) {
    mp_raise_msg(&mp_type_MemoryError, "Failed to create UREncoder");
  }

  // with_finaliser so __del__ (ur_encoder_del) runs on GC.
  mp_obj_ur_encoder_t *self = m_new_obj_with_finaliser(mp_obj_ur_encoder_t);
  self->base.type = type;
  self->encoder = encoder;
  self->fountain_encoder_cached = MP_OBJ_NULL;

  return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ur_encoder_del(mp_obj_t self_in) {
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);
  if (self->encoder) {
    ur_encoder_free(self->encoder);
    self->encoder = NULL;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_encoder_del_obj, ur_encoder_del);

// next_part method
static mp_obj_t ur_encoder_next_part_py(mp_obj_t self_in) {
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (!self->encoder) {
    mp_raise_msg(&mp_type_RuntimeError, "UREncoder is closed");
  }

  char *ur_part = NULL;
  bool result = ur_encoder_next_part(self->encoder, &ur_part);

  if (!result) {
    if (ur_part) {
      free(ur_part);
    }
    mp_raise_msg(&mp_type_RuntimeError, "Failed to generate next part");
  }

  if (!ur_part) {
    mp_raise_msg(&mp_type_RuntimeError, "Generated NULL part");
  }

  // Copy the string into MicroPython's memory space
  size_t len = strlen(ur_part);
  mp_obj_t part_str =
      mp_obj_new_str_copy(&mp_type_str, (const byte *)ur_part, len);

  // Free the C-allocated string
  free(ur_part);

  return part_str;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_encoder_next_part_obj,
                                 ur_encoder_next_part_py);

// is_complete method
static mp_obj_t ur_encoder_is_complete_py(mp_obj_t self_in) {
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->encoder) {
    return mp_const_false;
  }
  return mp_obj_new_bool(ur_encoder_is_complete(self->encoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_encoder_is_complete_obj,
                                 ur_encoder_is_complete_py);

// is_single_part method
static mp_obj_t ur_encoder_is_single_part_py(mp_obj_t self_in) {
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);
  if (!self->encoder) {
    return mp_const_false;
  }
  return mp_obj_new_bool(ur_encoder_is_single_part(self->encoder));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ur_encoder_is_single_part_obj,
                                 ur_encoder_is_single_part_py);

// UREncoder locals dict
static const mp_rom_map_elem_t ur_encoder_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ur_encoder_del_obj)},
    {MP_ROM_QSTR(MP_QSTR_next_part), MP_ROM_PTR(&ur_encoder_next_part_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_complete), MP_ROM_PTR(&ur_encoder_is_complete_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_single_part),
     MP_ROM_PTR(&ur_encoder_is_single_part_obj)},
};
static MP_DEFINE_CONST_DICT(ur_encoder_locals_dict,
                            ur_encoder_locals_dict_table);

// FountainEncoder wrapper type for accessing seq_len(). Holds a strong
// reference to the parent UREncoder Python object so GC cannot finalise
// the parent (and free ->encoder) while this wrapper is still reachable.
// The encoder pointer is always re-read from parent->encoder at call time
// so that an explicit parent.__del__() cleanly turns all wrapper reads
// into no-ops instead of dangling dereferences.
typedef struct {
  mp_obj_base_t base;
  mp_obj_t parent; // UREncoder Python object; MP_OBJ_NULL if detached
} mp_obj_fountain_encoder_wrapper_t;

// Reach through the parent back-ref to the live ur_encoder_t, or NULL if
// the parent was explicitly closed (ur_encoder_del set parent->encoder to
// NULL) or never attached.
static ur_encoder_t *
fountain_encoder_wrapper_encoder(mp_obj_fountain_encoder_wrapper_t *self) {
  if (self->parent == MP_OBJ_NULL)
    return NULL;
  mp_obj_ur_encoder_t *parent = MP_OBJ_TO_PTR(self->parent);
  return parent->encoder;
}

static void fountain_encoder_wrapper_print(const mp_print_t *print,
                                           mp_obj_t self_in,
                                           mp_print_kind_t kind) {
  (void)kind;
  mp_obj_fountain_encoder_wrapper_t *self = MP_OBJ_TO_PTR(self_in);
  ur_encoder_t *enc = fountain_encoder_wrapper_encoder(self);
  if (enc && enc->fountain_encoder) {
    mp_printf(print, "FountainEncoder(seq_len=%u)", ur_encoder_seq_len(enc));
  } else {
    mp_printf(print, "FountainEncoder(invalid)");
  }
}

static mp_obj_t fountain_encoder_seq_len_py(mp_obj_t self_in) {
  mp_obj_fountain_encoder_wrapper_t *self = MP_OBJ_TO_PTR(self_in);
  ur_encoder_t *enc = fountain_encoder_wrapper_encoder(self);
  if (!enc || !enc->fountain_encoder) {
    return mp_obj_new_int(0);
  }
  return mp_obj_new_int(ur_encoder_seq_len(enc));
}
static MP_DEFINE_CONST_FUN_OBJ_1(fountain_encoder_seq_len_obj,
                                 fountain_encoder_seq_len_py);

static const mp_rom_map_elem_t fountain_encoder_wrapper_locals_dict_table[] = {
    {MP_ROM_QSTR(MP_QSTR_seq_len), MP_ROM_PTR(&fountain_encoder_seq_len_obj)},
};
static MP_DEFINE_CONST_DICT(fountain_encoder_wrapper_locals_dict,
                            fountain_encoder_wrapper_locals_dict_table);

static const mp_obj_type_t mp_type_fountain_encoder_wrapper = {
    {&mp_type_type},
    .name = MP_QSTR_FountainEncoder,
    .print = fountain_encoder_wrapper_print,
    .locals_dict = (mp_obj_dict_t *)&fountain_encoder_wrapper_locals_dict,
};

// UREncoder attributes
static void ur_encoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
  mp_obj_ur_encoder_t *self = MP_OBJ_TO_PTR(self_in);

  if (dest[0] == MP_OBJ_NULL) {
    // Load attribute
    if (attr == MP_QSTR_fountain_encoder) {
      // Return cached fountain encoder wrapper to prevent memory leaks
      if (!self->encoder || !self->encoder->fountain_encoder) {
        dest[0] = mp_const_none;
        return;
      }

      // If not cached yet, create and cache the wrapper. The wrapper keeps
      // a strong ref to self so GC can't free self->encoder out from under it,
      // and reads encoder through self->parent->encoder each call so explicit
      // __del__ on the parent neuters the wrapper instead of dangling.
      if (self->fountain_encoder_cached == MP_OBJ_NULL) {
        mp_obj_fountain_encoder_wrapper_t *fe_wrapper =
            m_new_obj(mp_obj_fountain_encoder_wrapper_t);
        fe_wrapper->base.type = &mp_type_fountain_encoder_wrapper;
        fe_wrapper->parent = self_in;
        self->fountain_encoder_cached = MP_OBJ_FROM_PTR(fe_wrapper);
      }

      dest[0] = self->fountain_encoder_cached;
    } else {
      // Method lookup from locals_dict. Use the method-load protocol
      // (dest[0]=method, dest[1]=self) instead of allocating a bound
      // method — this path is hit by the GC finaliser looking up __del__,
      // and allocating during a sweep can trigger nlr_jump_fail.
      mp_obj_dict_t *locals_dict = (mp_obj_dict_t *)&ur_encoder_locals_dict;
      mp_map_elem_t *elem = mp_map_lookup(&locals_dict->map,
                                          MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
      if (elem != NULL) {
        dest[0] = elem->value;
        dest[1] = self_in;
      }
    }
  }
}

// UREncoder type definition
static const mp_obj_type_t mp_type_ur_encoder = {
    {&mp_type_type},
    .name = MP_QSTR_UREncoder,
    .print = ur_encoder_print,
    .make_new = ur_encoder_make_new,
    .attr = ur_encoder_attr,
    .locals_dict = (mp_obj_dict_t *)&ur_encoder_locals_dict,
};

// ============================================================================
// Types Module (for URTypes functionality)
// ============================================================================

// bytes_from_cbor(cbor_data) - module function
static mp_obj_t bytes_from_cbor_py(mp_obj_t cbor_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(cbor_data_in, &bufinfo, MP_BUFFER_READ);

  // Decode CBOR to Bytes
  bytes_data_t *bytes =
      bytes_from_cbor((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!bytes) {
    mp_raise_msg(&mp_type_ValueError, "Failed to decode Bytes from CBOR");
  }

  // Get raw bytes data
  size_t len;
  const uint8_t *data = bytes_get_data(bytes, &len);

  // Create Python bytes object
  mp_obj_t result = mp_obj_new_bytes(data, len);

  // Cleanup
  bytes_free(bytes);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bytes_from_cbor_obj, bytes_from_cbor_py);

// bytes_to_cbor(bytes_data) - module function
static mp_obj_t bytes_to_cbor_py(mp_obj_t bytes_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(bytes_data_in, &bufinfo, MP_BUFFER_READ);

  // Create Bytes from raw bytes
  bytes_data_t *bytes = bytes_new((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!bytes) {
    mp_raise_msg(&mp_type_MemoryError, "Failed to create Bytes");
  }

  // Encode to CBOR
  size_t cbor_len;
  uint8_t *cbor_data = bytes_to_cbor(bytes, &cbor_len);
  if (!cbor_data) {
    bytes_free(bytes);
    mp_raise_msg(&mp_type_RuntimeError, "Failed to encode Bytes to CBOR");
  }

  // Create Python bytes object
  mp_obj_t result = mp_obj_new_bytes(cbor_data, cbor_len);

  // Cleanup
  free(cbor_data);
  bytes_free(bytes);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bytes_to_cbor_obj, bytes_to_cbor_py);

// ============================================================================
// PSBT Functions
// ============================================================================

// psbt_from_cbor(cbor_data) - module function
static mp_obj_t psbt_from_cbor_py(mp_obj_t cbor_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(cbor_data_in, &bufinfo, MP_BUFFER_READ);

  // Decode CBOR to PSBT
  psbt_data_t *psbt = psbt_from_cbor((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!psbt) {
    mp_raise_msg(&mp_type_ValueError, "Failed to decode PSBT from CBOR");
  }

  // Get raw PSBT data
  size_t len;
  const uint8_t *data = psbt_get_data(psbt, &len);

  // Create Python bytes object
  mp_obj_t result = mp_obj_new_bytes(data, len);

  // Cleanup
  psbt_free(psbt);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(psbt_from_cbor_obj, psbt_from_cbor_py);

// psbt_to_cbor(psbt_data) - module function
static mp_obj_t psbt_to_cbor_py(mp_obj_t psbt_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(psbt_data_in, &bufinfo, MP_BUFFER_READ);

  // Create PSBT from raw bytes
  psbt_data_t *psbt = psbt_new((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!psbt) {
    mp_raise_msg(&mp_type_MemoryError, "Failed to create PSBT");
  }

  // Encode to CBOR
  size_t cbor_len;
  uint8_t *cbor_data = psbt_to_cbor(psbt, &cbor_len);
  if (!cbor_data) {
    psbt_free(psbt);
    mp_raise_msg(&mp_type_RuntimeError, "Failed to encode PSBT to CBOR");
  }

  // Create Python bytes object
  mp_obj_t result = mp_obj_new_bytes(cbor_data, cbor_len);

  // Cleanup
  free(cbor_data);
  psbt_free(psbt);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(psbt_to_cbor_obj, psbt_to_cbor_py);

// ============================================================================
// BIP39 Functions
// ============================================================================

// BIP39.words_from_cbor(cbor_data) - static function
static mp_obj_t bip39_words_from_cbor_py(mp_obj_t cbor_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(cbor_data_in, &bufinfo, MP_BUFFER_READ);

  // Decode CBOR to BIP39
  bip39_data_t *bip39 =
      bip39_from_cbor((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!bip39) {
    mp_raise_msg(&mp_type_ValueError, "Failed to decode BIP39 from CBOR");
  }

  // Get words
  size_t word_count;
  char **words = bip39_get_words(bip39, &word_count);

  // Create Python list
  mp_obj_t list = mp_obj_new_list(0, NULL);
  for (size_t i = 0; i < word_count; i++) {
    mp_obj_list_append(list, mp_obj_new_str(words[i], strlen(words[i])));
  }

  // Cleanup
  bip39_free(bip39);

  return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bip39_words_from_cbor_obj,
                                 bip39_words_from_cbor_py);

// ============================================================================
// Output Descriptor Functions
// ============================================================================

// output_from_cbor(cbor_data) - module function
static mp_obj_t output_from_cbor_py(mp_obj_t cbor_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(cbor_data_in, &bufinfo, MP_BUFFER_READ);

  // Decode CBOR to Output
  output_data_t *output =
      output_from_cbor((const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!output) {
    mp_raise_msg(&mp_type_ValueError, "Failed to decode Output from CBOR");
  }

  // Generate descriptor string with checksum
  char *descriptor = output_descriptor(output, true);
  if (!descriptor) {
    output_free(output);
    mp_raise_msg(&mp_type_RuntimeError, "Failed to generate output descriptor");
  }

  // Create Python string from descriptor
  mp_obj_t result = mp_obj_new_str(descriptor, strlen(descriptor));

  // Cleanup
  free(descriptor);
  output_free(output);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(output_from_cbor_obj, output_from_cbor_py);

// output_from_cbor_account(cbor_data) - module function
static mp_obj_t output_from_cbor_account_py(mp_obj_t cbor_data_in) {
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(cbor_data_in, &bufinfo, MP_BUFFER_READ);

  // Extract first output descriptor from Account CBOR
  char *descriptor = output_descriptor_from_cbor_account(
      (const uint8_t *)bufinfo.buf, bufinfo.len);
  if (!descriptor) {
    mp_raise_msg(&mp_type_ValueError,
                 "Failed to extract output descriptor from Account CBOR");
  }

  // Create Python string from descriptor
  mp_obj_t result = mp_obj_new_str(descriptor, strlen(descriptor));

  // Cleanup
  free(descriptor);

  return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(output_from_cbor_account_obj,
                                 output_from_cbor_account_py);

// ============================================================================
// Type String Constants (for UR type names with hyphens)
// ============================================================================

// Static string constants for UR type names (with hyphens)
static const mp_obj_str_t crypto_psbt_type_str = {
    {&mp_type_str}, 0, 11, (const byte *)"crypto-psbt"};
static const mp_obj_str_t crypto_bip39_type_str = {
    {&mp_type_str}, 0, 12, (const byte *)"crypto-bip39"};
static const mp_obj_str_t crypto_output_type_str = {
    {&mp_type_str}, 0, 13, (const byte *)"crypto-output"};
static const mp_obj_str_t crypto_account_type_str = {
    {&mp_type_str}, 0, 14, (const byte *)"crypto-account"};

// Types namespace globals
static const mp_rom_map_elem_t types_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_Types)},
    // Functions
    {MP_ROM_QSTR(MP_QSTR_bytes_from_cbor), MP_ROM_PTR(&bytes_from_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_bytes_to_cbor), MP_ROM_PTR(&bytes_to_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_psbt_from_cbor), MP_ROM_PTR(&psbt_from_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_psbt_to_cbor), MP_ROM_PTR(&psbt_to_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_bip39_words_from_cbor),
     MP_ROM_PTR(&bip39_words_from_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_output_from_cbor), MP_ROM_PTR(&output_from_cbor_obj)},
    {MP_ROM_QSTR(MP_QSTR_output_from_cbor_account),
     MP_ROM_PTR(&output_from_cbor_account_obj)},
    // Tag constants (integers)
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_PSBT_TAG), MP_ROM_INT(CRYPTO_PSBT_TAG)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_BIP39_TAG), MP_ROM_INT(CRYPTO_BIP39_TAG)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_ACCOUNT_TAG), MP_ROM_INT(CRYPTO_ACCOUNT_TAG)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_OUTPUT_TAG), MP_ROM_INT(CRYPTO_OUTPUT_TAG)},
    // Type name constants (strings with hyphens for UR type field)
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_PSBT_TYPE), MP_ROM_PTR(&crypto_psbt_type_str)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_BIP39_TYPE),
     MP_ROM_PTR(&crypto_bip39_type_str)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_OUTPUT_TYPE),
     MP_ROM_PTR(&crypto_output_type_str)},
    {MP_ROM_QSTR(MP_QSTR_CRYPTO_ACCOUNT_TYPE),
     MP_ROM_PTR(&crypto_account_type_str)},
};
static MP_DEFINE_CONST_DICT(types_globals, types_globals_table);

static const mp_obj_module_t types_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&types_globals,
};

// ============================================================================
// Main uUR Module
// ============================================================================

// Module globals table
static const mp_rom_map_elem_t bc_ur_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bc_ur)},
    {MP_ROM_QSTR(MP_QSTR_URDecoder), MP_ROM_PTR(&mp_type_ur_decoder)},
    {MP_ROM_QSTR(MP_QSTR_UREncoder), MP_ROM_PTR(&mp_type_ur_encoder)},
    {MP_ROM_QSTR(MP_QSTR_UR), MP_ROM_PTR(&mp_type_ur)},
    {MP_ROM_QSTR(MP_QSTR_Types), MP_ROM_PTR(&types_module)},
};
static MP_DEFINE_CONST_DICT(bc_ur_globals, bc_ur_globals_table);

// Module definition
const mp_obj_module_t bc_ur_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&bc_ur_globals,
};

MP_REGISTER_MODULE(MP_QSTR_uUR, bc_ur_module, MODULE_BC_UR_ENABLED);