/*
 * This is an implementation of the HSS signature scheme from LMS
 * This is designed to be full-featured
 *
 * Currently, this file consists of functions that don't have a better home
 */
#include <stdlib.h>
#include <string.h>
#include "common_defs.h"
#include "hss.h"
#include "hash.h"
#include "endian.h"
#include "hss_internal.h"
#include "hss_aux.h"
#include "hss_derive.h"
#include "hss_fault.h"

/*
 * Allocate and load an ephemeral key
 */
struct hss_working_key *hss_load_private_key(
    bool (*read_private_key)(unsigned char *private_key,
            size_t len_private_key, void *context),
    bool (*update_private_key)(unsigned char *private_key,
            size_t len_private_key, void *context),
        void *context,
    size_t memory_target,
    const unsigned char *aux_data, size_t len_aux_data,
    struct hss_extra_info *info ) {

    /* Step 1: determine the parameter set */
    unsigned levels;
    param_set_t lm[ MAX_HSS_LEVELS ];
    param_set_t ots[ MAX_HSS_LEVELS ];
    if (!hss_get_parameter_set( &levels, lm, ots, read_private_key, context,
                                 info)) {
        /* Can't read private key, or private key invalid */
        return 0;
    }

    /* Step 2: allocate the ephemeral key */
    struct hss_working_key *w = allocate_working_key(levels, lm, ots,
                                                 memory_target, info);
    if (!w) {
        /* Memory allocation failure, most likely (we've already vetted */
        /* the parameter sets) */
        return 0;
    }

    /* Step 3: load the ephemeral key */
    if (! hss_generate_working_key( read_private_key,
                                    update_private_key,
                                    context, 
                                    aux_data, len_aux_data, w, info )) {
        /* About the only thing I can see failing here is perhaps */
        /* attempting to reread the private key failed the second time; */
        /* seems unlikely, but not impossible */
        hss_free_working_key( w );
        return 0;
    }

    /* Success! */
    return w;
}

/*
 * Routines to read/update the private key
 */

/*
 * This computes the checksum that appears in the private key
 * It is here to detect write errors that might accidentally send us
 * backwards.  It is unkeyed, because we have no good place to get the
 * key from (if we assume the attacker can modify the private key, well,
 * we're out of luck)
 */
static void compute_private_key_checksum(
                  unsigned char checksum[PRIVATE_KEY_CHECKSUM_LEN],
                  const unsigned char *private_key ) {
    union hash_context ctx;
    unsigned char hash[MAX_HASH];

        /* Hash everything except the checksum */
    hss_set_level(0);
    hss_set_hash_reason(h_reason_priv_checksum);
    hss_init_hash_context( HASH_SHA256, &ctx );
    hss_update_hash_context( HASH_SHA256, &ctx,
                             private_key, PRIVATE_KEY_CHECKSUM );
    hss_update_hash_context( HASH_SHA256, &ctx,
             private_key + PRIVATE_KEY_CHECKSUM + PRIVATE_KEY_CHECKSUM_LEN,
             PRIVATE_KEY_LEN -
                    (PRIVATE_KEY_CHECKSUM + PRIVATE_KEY_CHECKSUM_LEN ));
    hss_finalize_hash_context( HASH_SHA256, &ctx,
             hash );

        /* The first 8 bytes of the hash is the checksum */
    memcpy( checksum, hash, PRIVATE_KEY_CHECKSUM_LEN );

    hss_zeroize( &ctx, sizeof ctx );
    hss_zeroize( hash, sizeof hash );
}

static const unsigned char expected_format[ PRIVATE_KEY_FORMAT_LEN ] = {
    0x01,  /* Current format version */
    SECRET_METHOD ? SECRET_MAX : 0xff,  /* Secret method marker */
    0,     /* Reserved for future use */
    0
};

void hss_set_private_key_format(unsigned char *private_key) {
    memcpy( private_key + PRIVATE_KEY_FORMAT, expected_format,
            PRIVATE_KEY_FORMAT_LEN );
}

bool hss_check_private_key(const unsigned char *private_key) {
    /* If the key isn't in the format we expect, it's a bad key (or, at */
    /* least, it's unusable by us) */
    if (0 != memcmp( private_key + PRIVATE_KEY_FORMAT, expected_format,
                                                 PRIVATE_KEY_FORMAT_LEN )) {
        return false;
    }

    /* Check the checksum on the key */
    unsigned char checksum[ PRIVATE_KEY_CHECKSUM_LEN ];
    compute_private_key_checksum( checksum, private_key );
    bool success = (0 == memcmp( checksum, &private_key[PRIVATE_KEY_CHECKSUM],
                     PRIVATE_KEY_CHECKSUM_LEN ));
    hss_zeroize( checksum, sizeof checksum );
    return success;
}

enum hss_error_code hss_read_private_key(unsigned char *private_key,
            struct hss_working_key *w) {
    if (w->read_private_key) {
        if (!w->read_private_key( private_key, PRIVATE_KEY_LEN, w->context)) {
            hss_zeroize( private_key, PRIVATE_KEY_LEN );
            return hss_error_private_key_read_failed;
        }
    } else {
        memcpy( private_key, w->context, PRIVATE_KEY_LEN );
    }

    if (!hss_check_private_key(private_key)) { 
        hss_zeroize( private_key, PRIVATE_KEY_LEN );
        return hss_error_bad_private_key;
    }
    return hss_error_none;
}

/*
 * This assumes that the private key is already set up, and so only updates
 * the counter and the checksum
 */
enum hss_error_code hss_write_private_key(unsigned char *private_key,
            struct hss_working_key *w) {
    return hss_write_private_key_no_w( private_key,
              PRIVATE_KEY_CHECKSUM + PRIVATE_KEY_CHECKSUM_LEN, 
              w->read_private_key, w->update_private_key, w->context );
}

enum hss_error_code hss_write_private_key_no_w(
            unsigned char *private_key, size_t len,
            bool (*read_private_key)(unsigned char *private_key,
                                    size_t len_private_key, void *context),
            bool (*update_private_key)(unsigned char *private_key,
                                    size_t len_private_key, void *context),
            void *context) {
    /* Update the checksum */
    compute_private_key_checksum( private_key + PRIVATE_KEY_CHECKSUM,
                                  private_key );

    /* Write it out */
    if (update_private_key) {
        if (!update_private_key( private_key, len, context )) {
            return hss_error_private_key_write_failed;
        }
#if FAULT_HARDENING
        /* Double check that the write went through */
        /* Note: read_private_key is null only during the initial write */
        /* during key generation; errors there don't break security */
        /* Q: this is relatively cheap; should we do this even if */
        /*    !FAULT_HARDENING ??? */
        if (read_private_key) {
            unsigned char private_key_check[PRIVATE_KEY_LEN];
            if (!read_private_key( private_key_check, PRIVATE_KEY_LEN,
                                   context )) {
                hss_zeroize( private_key_check, PRIVATE_KEY_LEN );
                return hss_error_private_key_read_failed;
            }
            int cmp = memcmp( private_key, private_key_check,
                              PRIVATE_KEY_LEN );
            hss_zeroize( private_key_check, PRIVATE_KEY_LEN );
            if (cmp != 0) {
                 return hss_error_bad_private_key;
            }  
        }
#endif
    } else {
        memcpy( context, private_key, len );
    }

    return hss_error_none;
}

/*
 * Internal function to generate the root seed and I value (based on the
 * private seed).  We do this (rather than select seed, I at random) so that
 * we don't need to store it in our private key; we can recompute them
 *
 * We use a two-level hashing scheme so that we end up using the master seed
 * only twice throughout the system (once here, once to generate the aux
 * hmac key)
 */
void hss_generate_root_seed_I_value(unsigned char *seed, unsigned char *I,
                                    const unsigned char *master_seed) {
    unsigned char hash_preimage[ TOPSEED_LEN ];
    unsigned char hash_postimage[ MAX_HASH ];

    memset( hash_preimage + TOPSEED_I, 0, I_LEN );
    memset( hash_preimage + TOPSEED_Q, 0, 4 );
    SET_D( hash_preimage + TOPSEED_D, D_TOPSEED );
    hash_preimage[TOPSEED_WHICH] = 0x00;
    memcpy( hash_preimage + TOPSEED_SEED, master_seed, SEED_LEN );

        /* We use a fixed SHA256 hash; we don't care about interoperability */
        /* so we don't need to worry about what parameter set the */
        /* user specified */
#if I_LEN > 32 || SEED_LEN != 32
#error This logic needs to be reworked
#endif
    union hash_context ctx;

    hss_set_level(0);
    hss_set_hash_reason(h_reason_other);

    hss_hash_ctx(hash_postimage, HASH_SHA256, &ctx, hash_preimage,
                                                            TOPSEED_LEN );
    memcpy( hash_preimage + TOPSEED_SEED, hash_postimage, SEED_LEN );

    /* Now compute the top level seed */
    hash_preimage[TOPSEED_WHICH] = 0x01;
    hss_hash_ctx(seed, HASH_SHA256, &ctx, hash_preimage, TOPSEED_LEN );

    /* Now compute the top level I value */
    hash_preimage[TOPSEED_WHICH] = 0x02;
    hss_hash_ctx(hash_postimage, HASH_SHA256, &ctx, hash_preimage,
                                                            TOPSEED_LEN );
    memcpy( I, hash_postimage, I_LEN );

    hss_zeroize( hash_preimage, sizeof hash_preimage );  /* There's keying */
                                                       /* data here */
    hss_zeroize( &ctx, sizeof ctx );
}

/*
 * Internal function to generate the child I value (based on the parent's
 * I value).  While this needs to be determanistic (so that we can create the
 * same I values between reboots), there's no requirement for interoperability.
 * So we use a fixed SHA256; when we support a hash function other than SHA256,
 * we needn't update this.
 */
void hss_generate_child_seed_I_value( unsigned char *seed, unsigned char *I,
                   const unsigned char *parent_seed,
                   const unsigned char *parent_I,
                   merkle_index_t index,
                   param_set_t lm, param_set_t ots, int child_level) {
    hss_set_level(child_level);
    struct seed_derive derive;
    if (!hss_seed_derive_init( &derive, lm, ots, parent_I, parent_seed )) {
        return;
    }

    hss_seed_derive_set_q( &derive, index );

    /* Compute the child seed value */
    hss_seed_derive_set_j( &derive, SEED_CHILD_SEED );
    hss_seed_derive( seed, &derive, true );
        /* True sets the j value to SEED_CHILD_I */
   
    /* Compute the child I value */ 
    unsigned char postimage[ SEED_LEN ];
    hss_seed_derive( postimage, &derive, false );
    memcpy( I, postimage, I_LEN );

    hss_seed_derive_done( &derive );
}

void hss_init_extra_info( struct hss_extra_info *p ) {
    if (p) memset( p, 0, sizeof *p );
}

void hss_extra_info_set_threads( struct hss_extra_info *p, int num_threads ) {
    if (p) p->num_threads = num_threads;
}

bool hss_extra_info_test_last_signature( struct hss_extra_info *p ) {
    if (!p) return false;
    return p->last_signature;
}

enum hss_error_code hss_extra_info_test_error_code( struct hss_extra_info *p ) {
    if (!p) return hss_error_got_null;
    return p->error_code;
}

bool hss_is_fault_hardening_on(void) {
    return FAULT_HARDENING;
}
