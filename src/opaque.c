/*
    @copyright 2018-21, opaque@ctrlc.hu
    This file is part of libopaque.

    libopaque is free software: you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    libopaque is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with libopaque. If not, see <http://www.gnu.org/licenses/>.

    This file implements the Opaque protocol as specified by the IRTF CFRG
*/

#include "opaque.h"
#include <oprf/oprf.h>

#if _WIN32 == 1 || _WIN64 == 1
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include "common.h"
#ifdef CFRG_TEST_VEC
#include "tests/cfrg_test_vector_decl.h"
#endif

#ifndef HAVE_SODIUM_HKDF
#include "aux_/crypto_kdf_hkdf_sha512.h"
#endif

#define VOPRF "OPRFV1"

#define OPAQUE_RWDU_BYTES 64
#define OPAQUE_HANDSHAKE_SECRETBYTES 64
#define OPAQUE_NONCE_BYTES 32
#define OPAQUE_ENVELOPE_BYTES (OPAQUE_ENVELOPE_NONCEBYTES + crypto_auth_hmacsha512_BYTES)
#define OPAQUE_HMAC_SHA512_BYTES 64
#define OPAQUE_HMAC_SHA512_KEYBYTES 64

#define __bounds(member) __attribute__((__element_count__(member)))

typedef struct {
  uint8_t nonce[OPAQUE_ENVELOPE_NONCEBYTES];
  uint8_t auth_tag[crypto_auth_hmacsha512_BYTES];
} __attribute((packed)) Opaque_Envelope;

typedef struct {
  uint8_t client_public_key[crypto_scalarmult_BYTES];
  uint8_t masking_key[crypto_hash_sha512_BYTES];
  Opaque_Envelope envelope;
} __attribute((packed)) Opaque_RegistrationRecord;


// user specific record stored at server upon registration
typedef struct {
  uint8_t kU[crypto_core_ristretto255_SCALARBYTES];
  uint8_t skS[crypto_scalarmult_SCALARBYTES];
  Opaque_RegistrationRecord recU;
} __attribute((packed)) Opaque_UserRecord;

typedef struct {
  uint8_t blinded[crypto_core_ristretto255_BYTES];
  uint8_t nonceU[OPAQUE_NONCE_BYTES];
  uint8_t X_u[crypto_scalarmult_BYTES];
} __attribute((packed)) Opaque_UserSession;

typedef struct {
  uint8_t blind[crypto_core_ristretto255_SCALARBYTES];
  uint8_t x_u[crypto_scalarmult_SCALARBYTES];
  uint8_t nonceU[OPAQUE_NONCE_BYTES];
  uint8_t blinded[crypto_core_ristretto255_BYTES];
  uint8_t ke1[OPAQUE_USER_SESSION_PUBLIC_LEN];
  uint16_t pwdU_len;
  uint8_t pwdU[] __bounds(pwdU_len);
} __attribute((packed)) Opaque_UserSession_Secret;

typedef struct {
  uint8_t Z[crypto_core_ristretto255_BYTES];
  uint8_t masking_nonce[32];
  uint8_t masked_response[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  uint8_t nonceS[OPAQUE_NONCE_BYTES];
  uint8_t X_s[crypto_scalarmult_BYTES];
  uint8_t auth[crypto_auth_hmacsha512_BYTES];
} __attribute((packed)) Opaque_ServerSession;

typedef struct {
  uint8_t blind[crypto_core_ristretto255_SCALARBYTES];
  uint16_t pwdU_len;
  uint8_t pwdU[] __bounds(pwdU_len);
} Opaque_RegisterUserSec;

typedef struct {
  uint8_t Z[crypto_core_ristretto255_BYTES];
  uint8_t pkS[crypto_scalarmult_BYTES];
} __attribute((packed)) Opaque_RegisterSrvPub;

typedef struct {
  uint8_t skS[crypto_scalarmult_SCALARBYTES];
  uint8_t kU[crypto_core_ristretto255_SCALARBYTES];
} __attribute((packed)) Opaque_RegisterSrvSec;

typedef struct {
  uint8_t sk[OPAQUE_SHARED_SECRETBYTES];
  uint8_t km2[OPAQUE_HMAC_SHA512_KEYBYTES];
  uint8_t km3[OPAQUE_HMAC_SHA512_KEYBYTES];
} __attribute((packed)) Opaque_Keys;

// sodium defines an hmac with 32B as key, opaque as 64
static void opaque_hmacsha512(const uint8_t *key,
                              const uint8_t *authenticated, const size_t auth_len,
                              uint8_t *mac) {
  crypto_auth_hmacsha512_state st;
  crypto_auth_hmacsha512_init(&st, key, OPAQUE_HMAC_SHA512_KEYBYTES);
  crypto_auth_hmacsha512_update(&st, authenticated, auth_len);
  crypto_auth_hmacsha512_final(&st, mac);
  sodium_memzero(&st,sizeof st);
}

static int voprf_hash_to_scalar(const uint8_t *msg, const uint8_t msg_len, const uint8_t *dst, const uint8_t dst_len, uint8_t p[crypto_core_ristretto255_SCALARBYTES]) {
  //const uint8_t dst[] = "HashToScalar-"VOPRF"-\x00\x00\x01";
  //const uint8_t dst_len = (sizeof dst) - 1;
  uint8_t uniform_bytes[crypto_core_ristretto255_HASHBYTES]={0};
  if(0!=sodium_mlock(uniform_bytes,sizeof uniform_bytes)) {
    return -1;
  }
  if(0!=expand_message_xmd(msg, msg_len, dst, dst_len, crypto_core_ristretto255_HASHBYTES, uniform_bytes)) {
    sodium_munlock(uniform_bytes,sizeof uniform_bytes);
    return -1;
  }
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(uniform_bytes, sizeof uniform_bytes, "uniform_bytes");
#endif
  crypto_core_ristretto255_scalar_reduce(p, uniform_bytes);
  sodium_munlock(uniform_bytes,sizeof uniform_bytes);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(p, crypto_core_ristretto255_BYTES, "hashed-to-scalar");
#endif
  return 0;
}

static int deriveKeyPair(const uint8_t *seed, const size_t seed_len, uint8_t skS[crypto_core_ristretto255_SCALARBYTES], uint8_t pkS[crypto_core_ristretto255_BYTES]) {
  const uint16_t info_len=33;
  const uint8_t info[]="OPAQUE-DeriveDiffieHellmanKeyPair";
  const uint8_t ctx[] = "DeriveKeyPair"VOPRF"-\x00-ristretto255-SHA512";
  uint8_t hashinput[seed_len + 2 + info_len + 1], *ptr= hashinput;
  memcpy(ptr,seed,seed_len);
  ptr+=seed_len;
  *((uint16_t*) ptr)=htons(info_len);
  ptr+=2;
  memcpy(ptr,info,info_len);
  ptr+=info_len;
  ptr[0]=0;
  memset(skS,0,crypto_core_ristretto255_SCALARBYTES);
  int i;
  while(1) {
    if(ptr[0]>16) return 1; // DeriveKeyPairError
    for(i=crypto_core_ristretto255_SCALARBYTES/sizeof(uint32_t);i>0;i--) {
      if((((uint32_t*)skS)[i-1])!=0) break;
    }
    if(i!=0) break;
    if(0!=voprf_hash_to_scalar(hashinput,sizeof hashinput, ctx, sizeof ctx -1,skS)) return -1;
    ptr[0]++;
  }

  // P_u := g^p_u
  crypto_scalarmult_ristretto255_base(pkS, skS);
  return 0;
}

static int finalize(const uint8_t *x, const uint16_t x_len,
                         const uint8_t N[crypto_core_ristretto255_BYTES],
                         uint8_t *rwdU) {

  // - concat(y, Harden(y, params))
  uint8_t concated[2*crypto_hash_sha512_BYTES];
  uint8_t *y=concated, *hardened=concated+crypto_hash_sha512_BYTES;
  if(-1==sodium_mlock(&concated,sizeof concated)) {
    return -1;
  }

  if(0!=oprf_Finalize(x, x_len, N, y)) return 1;

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump((uint8_t*) y, crypto_hash_sha512_BYTES, "oprf ");
#endif

#ifdef CFRG_TEST_VEC
  // testvectors use identity as MHF
  memcpy(hardened, y, crypto_hash_sha512_BYTES);
#else
  // salt - according to the irtf draft this could be all zeroes
  // TODO parametrize via params and fall back to default
  uint8_t salt[crypto_pwhash_SALTBYTES]={0};
  if (crypto_pwhash(hardened, crypto_hash_sha512_BYTES,
                    (const char*) y, crypto_hash_sha512_BYTES, salt,
                    crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE,
                    crypto_pwhash_ALG_DEFAULT) != 0) {
    /* out of memory */
    sodium_munlock(concated, sizeof(concated));
    return -1;
  }
#endif
#if (defined TRACE|| defined CFRG_TEST_VEC)
  dump(concated, sizeof concated, "concated");
#endif

  crypto_kdf_hkdf_sha512_extract(rwdU, NULL, 0, concated, sizeof concated);
  sodium_munlock(concated, sizeof(concated));

#if (defined TRACE|| defined CFRG_TEST_VEC)
  dump((uint8_t*) rwdU, OPRF_BYTES, "rwdU");
#endif
  return 0;
}

static int prf(const uint8_t *pwdU, const uint16_t pwdU_len,
               const uint8_t kU[crypto_core_ristretto255_SCALARBYTES],
               uint8_t *rwdU) {
  // F_k(pwd) = H(pwd, (H0(pwd))^k) for key k ∈ Z_q
  uint8_t H0[crypto_core_ristretto255_BYTES];
  if(0!=sodium_mlock(H0,sizeof H0)) {
    return -1;
  }
  // sets α := (H^0(pw))^r
  if(0!=voprf_hash_to_group(pwdU, pwdU_len, H0)) return -1;
#ifdef TRACE
  dump(H0,sizeof H0, "H0");
#endif

  // H0 ^ k
  uint8_t N[crypto_core_ristretto255_BYTES];
  if(0!=sodium_mlock(N,sizeof N)) {
    sodium_munlock(H0,sizeof H0);
    return -1;
  }
  if (crypto_scalarmult_ristretto255(N, kU, H0) != 0) {
    sodium_munlock(H0,sizeof H0);
    sodium_munlock(N,sizeof N);
    return -1;
  }
  sodium_munlock(H0,sizeof H0);
#ifdef TRACE
  dump(N, sizeof N, "N");
#endif

  // 2. rwdU = Finalize(pwdU, N, "OPAQUE01")
  if(0!=finalize(pwdU, pwdU_len, N, rwdU)) {
    sodium_munlock(N,sizeof N);
    return -1;
  }
  sodium_munlock(N,sizeof N);

  return 0;
}

static void hkdf_expand_label(uint8_t* res, const uint8_t secret[crypto_kdf_hkdf_sha512_KEYBYTES], const char *label, const char transcript[crypto_hash_sha512_BYTES], const size_t len) {
  // construct a hkdf label
  // struct {
  //   uint16 length = Length;
  //   opaque label<8..255> = "OPAQUE-" + Label;
  //   opaque context<0..255> = Context;
  // } HkdfLabel;
  const size_t llen = strlen((const char*) label);
  uint8_t hkdflabel[2+2+7/*"OPAQUE-"*/+llen+(transcript!=NULL?crypto_hash_sha512_BYTES:0)];

  *((uint16_t*) hkdflabel)=htons(len);

  uint8_t *ptr=hkdflabel+2;
  *(ptr)=(7+llen);
  ptr+=1;

  memcpy(ptr,"OPAQUE-",7);
  ptr+=7;

  memcpy(ptr,label,llen);
  ptr+=llen;

  if(transcript!=NULL) {
    *(ptr)=crypto_hash_sha512_BYTES;
    ptr+=1;
    memcpy(ptr, transcript, crypto_hash_sha512_BYTES);
  } else {
    *(ptr)=0;
  }

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(hkdflabel, sizeof(hkdflabel), "expanded label");
  if(transcript!=NULL) dump((const uint8_t*) transcript,crypto_hash_sha512_BYTES, "transcript: ");
#endif

  crypto_kdf_hkdf_sha512_expand(res, len, (const char*) hkdflabel, sizeof(hkdflabel), secret);
}

// derive keys according to irtf cfrg draft
static int derive_keys(Opaque_Keys* keys, const uint8_t ikm[crypto_scalarmult_BYTES * 3], const char info[crypto_hash_sha512_BYTES]) {
  uint8_t prk[64];
  if(-1==sodium_mlock(prk, sizeof prk)) return -1;
#ifdef TRACE
  dump(ikm, crypto_scalarmult_BYTES*3, "ikm ");
  dump((uint8_t*) info, crypto_hash_sha512_BYTES, "info ");
#endif
  // 1. prk = HKDF-Extract(salt=0, IKM)
  crypto_kdf_hkdf_sha512_extract(prk, NULL, 0, ikm, crypto_scalarmult_BYTES*3);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(prk, sizeof prk, "prk");
#endif

  // 2. handshake_secret = Derive-Secret(., "handshake secret", info)
  uint8_t handshake_secret[OPAQUE_HANDSHAKE_SECRETBYTES];
  if(-1==sodium_mlock(handshake_secret, sizeof handshake_secret)) {
    sodium_munlock(prk,sizeof(prk));
    return -1;
  }
  const char handshake_secret_label[]="HandshakeSecret";
  hkdf_expand_label(handshake_secret, prk, handshake_secret_label, info, sizeof(handshake_secret));

  // 3. keys->sk         = Derive-Secret(., "session secret", info)
  const char session_key_label[]="SessionKey";
  hkdf_expand_label(keys->sk, prk, session_key_label, info, OPAQUE_SHARED_SECRETBYTES);
  sodium_munlock(prk,sizeof(prk));

  // 4. Km2 = Derive-Secret(handshake_secret, "ServerMAC", "")
  //Km2 = HKDF-Expand-Label(handshake_secret, "server mac", "", Hash.length)
  const char server_mac_label[]="ServerMAC";
  hkdf_expand_label(keys->km2, handshake_secret, server_mac_label, NULL, OPAQUE_HMAC_SHA512_KEYBYTES);
  // 5. Km3 = Derive-Secret(handshake_secret, "ClientMAC", "")
  //Km3 = HKDF-Expand-Label(handshake_secret, "client mac", "", Hash.length)
  const char client_mac_label[]="ClientMAC";
  hkdf_expand_label(keys->km3, handshake_secret, client_mac_label, NULL, OPAQUE_HMAC_SHA512_KEYBYTES);
  sodium_munlock(handshake_secret, sizeof handshake_secret);
#ifdef TRACE
  dump(keys->sk, OPAQUE_SHARED_SECRETBYTES, "keys->sk");
  dump(keys->km2, OPAQUE_HMAC_SHA512_KEYBYTES, "keys->km2");
  dump(keys->km3, OPAQUE_HMAC_SHA512_KEYBYTES, "keys->km3");
#endif
  return 0;
}

/** if one of the peers ID is missing, set it to the peers public key */
static void fix_ids(const uint8_t pkU[crypto_scalarmult_BYTES],
                    const uint8_t pkS[crypto_scalarmult_BYTES],
                    const Opaque_Ids *ids_in,
                    Opaque_Ids *ids_out) {
  if(ids_in->idS==NULL || ids_in->idS_len==0) {
    ids_out->idS=(uint8_t*)pkS;
    ids_out->idS_len=crypto_scalarmult_BYTES;
  } else {
    ids_out->idS=ids_in->idS;
    ids_out->idS_len=ids_in->idS_len;
  }
  if(ids_in->idU==NULL || ids_in->idU_len==0) {
    ids_out->idU=(uint8_t*)pkU;
    ids_out->idU_len=crypto_scalarmult_BYTES;
  } else {
    ids_out->idU=ids_in->idU;
    ids_out->idU_len=ids_in->idU_len;
  }
}

static void calc_preamble(char preamble[crypto_hash_sha512_BYTES],
                          crypto_hash_sha512_state *state,
                          const uint8_t pkU[crypto_scalarmult_BYTES],
                          const uint8_t pkS[crypto_scalarmult_BYTES],
                          const uint8_t *ke1,
                          const Opaque_ServerSession *ke2,
                          const uint8_t *ctx, const uint16_t ctx_len,
                          const Opaque_Ids *ids0) {
  crypto_hash_sha512_init(state);

  Opaque_Ids ids;
  fix_ids(pkU, pkS, ids0, &ids);

#ifdef TRACE
  fprintf(stderr,"calc preamble\n");
  dump(ids.idU, ids.idU_len,"idU ");
  dump(ids.idS, ids.idS_len,"idS ");
  dump(pkU, crypto_scalarmult_BYTES, "pkU");
  dump(pkS,crypto_scalarmult_BYTES, "pkS");
  dump(ke1, OPAQUE_USER_SESSION_PUBLIC_LEN, "ke1");
  dump(ctx, ctx_len, "ctx");
  dump((uint8_t*)ke2,
       /* credential_response */
       /*Z*/ crypto_core_ristretto255_BYTES +
       /*masking_nonce*/ 32+
       /*masked_response*/ crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)+
       /*nonceS*/OPAQUE_NONCE_BYTES+
       /*X_s*/crypto_scalarmult_BYTES, "ke2");
#endif

  //1. preamble = hash("OPAQUEv1-",
  // note the spec it self does not say hash here, but
  // https://github.com/cfrg/draft-irtf-cfrg-opaque/pull/147
  // and later uses all hash this value
  const uint8_t rfc[]="OPAQUEv1-";
  const uint8_t rfc_len=sizeof rfc -1;
  crypto_hash_sha512_update(state, rfc, rfc_len);

  //                   I2OSP(len(context), 2), context,
  uint16_t len = htons(ctx_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ctx, ctx_len);

  //                   I2OSP(len(client_identity), 2), client_identity,
  len = htons(ids.idU_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ids.idU, ids.idU_len);

  //                   ke1,
  crypto_hash_sha512_update(state, ke1, OPAQUE_USER_SESSION_PUBLIC_LEN);

  //                   I2OSP(len(server_identity), 2), server_identity,
  len = htons(ids.idS_len);
  crypto_hash_sha512_update(state, (uint8_t*) &len, 2);
  crypto_hash_sha512_update(state, ids.idS, ids.idS_len);

  //                   ke2.credential_response,
  //                   ke2.AuthResponse.server_nonce, ke2.AuthResponse.server_keyshare)
  //  see type Opaque_ServerSession
  crypto_hash_sha512_update(state, (uint8_t*)ke2,
                            /* credential_response */
                            /*Z*/ crypto_core_ristretto255_BYTES +
                            /*masking_nonce*/ 32+
                            /*masked_response*/ crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)+
                            /*nonceS*/OPAQUE_NONCE_BYTES+
                            /*X_s*/crypto_scalarmult_BYTES);

  // We need to copy the state here, because the caller of this function
  // may re-use it later. After calling the `final` function below,
  // the passed-in state must not be used again.
  crypto_hash_sha512_state copied_state;
  memcpy(&copied_state, state, sizeof(crypto_hash_sha512_state));
  crypto_hash_sha512_final(&copied_state, (uint8_t *) preamble);
}

// implements server end of triple-dh
static int server_3dh(Opaque_Keys *keys,
               const uint8_t ix[crypto_scalarmult_SCALARBYTES],
               const uint8_t ex[crypto_scalarmult_SCALARBYTES],
               const uint8_t Ip[crypto_scalarmult_BYTES],
               const uint8_t Ep[crypto_scalarmult_BYTES],
               const char preamble[crypto_hash_sha512_BYTES]) {
  uint8_t sec[crypto_scalarmult_BYTES * 3], *ptr = sec;
  if(-1==sodium_mlock(sec, sizeof sec)) {
    return -1;
  }

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(ix, crypto_scalarmult_SCALARBYTES, "skS");
  dump(ex, crypto_scalarmult_SCALARBYTES, "ekS");
  dump(Ip, crypto_scalarmult_BYTES, "pkU");
  dump(Ep, crypto_scalarmult_BYTES, "epkU");
#endif

  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ix,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ip)) return 1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(sec, 96, "3dh s ikm");
#endif

  if(0!=derive_keys(keys, sec, preamble)) {
    sodium_munlock(sec,sizeof(sec));
    return -1;
  }
  sodium_munlock(sec,sizeof(sec));
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump((uint8_t*) keys, sizeof(Opaque_Keys), "keys ");
#endif

  return 0;
}

// implements user end of triple-dh
static int user_3dh(Opaque_Keys *keys,
             const uint8_t ix[crypto_scalarmult_SCALARBYTES],
             const uint8_t ex[crypto_scalarmult_SCALARBYTES],
             const uint8_t Ip[crypto_scalarmult_BYTES],
             const uint8_t Ep[crypto_scalarmult_BYTES],
             const char preamble[crypto_hash_sha512_BYTES]) {
  uint8_t sec[crypto_scalarmult_BYTES * 3], *ptr = sec;
  if(-1==sodium_mlock(sec, sizeof sec)) {
    return -1;
  }

  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ep)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ex,Ip)) return 1;
  ptr+=crypto_scalarmult_BYTES;
  if(0!=crypto_scalarmult_ristretto255(ptr,ix,Ep)) return 1;
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(sec, 96, "3dh u ikm");
#endif

  // and hash for the result SK = f_K(0)
  if(0!=derive_keys(keys, sec, preamble)) {
    sodium_munlock(sec,sizeof(sec));
    return -1;
  }
  sodium_munlock(sec,sizeof(sec));
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump((uint8_t*) keys, sizeof(Opaque_Keys), "keys ");
#endif

  return 0;
}

static int skU_from_rwd(const uint8_t *rwd, const uint8_t *nonce, uint8_t skU[crypto_scalarmult_BYTES]) {
  char info[OPAQUE_NONCE_BYTES+10];
  memcpy(info, nonce, OPAQUE_NONCE_BYTES);
  memcpy(info+OPAQUE_NONCE_BYTES, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  if(-1==sodium_mlock(seed, sizeof seed)) {
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES, info, sizeof info, rwd);

  uint8_t dst[24]="OPAQUE-DeriveAuthKeyPair";
  if(0!=voprf_hash_to_scalar(seed, sizeof seed, dst, sizeof dst, skU)) {
    sodium_munlock(seed, sizeof seed);
    return -1;
  }

  sodium_munlock(seed, sizeof seed);
  return 0;
}

static int create_envelope(const uint8_t *rwdU,
                           const uint8_t server_public_key[crypto_scalarmult_BYTES],
                           const Opaque_Ids *ids,
                           Opaque_Envelope *env,
                           uint8_t client_public_key[crypto_scalarmult_BYTES],
                           uint8_t masking_key[crypto_hash_sha512_BYTES],
                           uint8_t export_key[crypto_hash_sha512_BYTES]) {

  // 1. envelope_nonce = random(Nn)
#ifdef CFRG_TEST_VEC
  memcpy(env->nonce, envelope_nonce, envelope_nonce_len);
#else
  randombytes(env->nonce, OPAQUE_ENVELOPE_NONCEBYTES);
#endif

  uint8_t concated[OPAQUE_ENVELOPE_NONCEBYTES+10],
    *label = concated+OPAQUE_ENVELOPE_NONCEBYTES;
  memcpy(concated, env->nonce, OPAQUE_ENVELOPE_NONCEBYTES);

  // 2. masking_key = HKDF-Expand(randomized_pwd, "MaskingKey", Nh)
  const uint8_t masking_key_info[10]="MaskingKey";
  crypto_kdf_hkdf_sha512_expand(masking_key, crypto_hash_sha512_BYTES,
                                (const char*) masking_key_info, sizeof masking_key_info,
                                rwdU);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(masking_key_info, sizeof masking_key_info, "masking_key_info");
  dump(rwdU, OPAQUE_RWDU_BYTES, "rwdU");
  dump(masking_key, crypto_hash_sha512_BYTES, "masking_key");
#endif

  // 3. auth_key = HKDF-Expand(randomized_pwd, concat(envelope_nonce, "AuthKey"), Nh)
  uint8_t auth_key[OPAQUE_HMAC_SHA512_KEYBYTES];
  if(-1==sodium_mlock(auth_key, sizeof auth_key)) {
    return -1;
  }
  memcpy(label, "AuthKey", 7);
  crypto_kdf_hkdf_sha512_expand(auth_key, sizeof auth_key,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+7,
                                rwdU);

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(auth_key,sizeof auth_key, "auth_key ");
#endif

  // 4. export_key = HKDF-Expand(randomized_pwd, concat(envelope_nonce, "ExportKey"), Nh)
  if(NULL!=export_key) {
    memcpy(label, "ExportKey", 9);
#if (defined CFRG_TEST_VEC || defined TRACE)
    dump(concated, OPAQUE_ENVELOPE_NONCEBYTES+9, "export_key_info");
#endif
    crypto_kdf_hkdf_sha512_expand(export_key, crypto_hash_sha512_BYTES,
                                  (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+9,
                                  rwdU);
#if (defined CFRG_TEST_VEC || defined TRACE)
    dump(export_key,crypto_hash_sha512_BYTES, "export_key ");
#endif
  }

  // 5. seed = Expand(randomized_pwd, concat(envelope_nonce, "PrivateKey"), Nseed)
  memcpy(label, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  if(-1==sodium_mlock(seed, sizeof seed)) {
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+10,
                                rwdU);

  // 6. _, client_public_key = DeriveAuthKeyPair(seed)
  uint8_t client_secret_key[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(client_secret_key, sizeof client_secret_key)) {
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  if(0!=deriveKeyPair(seed, sizeof seed, client_secret_key, client_public_key)) {
    sodium_munlock(seed, sizeof seed);
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  sodium_munlock(seed, sizeof seed);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_secret_key, crypto_scalarmult_SCALARBYTES, "client_secret_key");
#endif
  sodium_munlock(client_secret_key, sizeof client_secret_key);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_public_key, crypto_scalarmult_BYTES, "client_public_key");
#endif

  // complete ids in case they are NULL and need to be set to the pk[US]
  Opaque_Ids ids_completed;
  fix_ids(client_public_key, server_public_key, ids, &ids_completed);

  uint8_t authenticated[OPAQUE_NONCE_BYTES+
                        crypto_scalarmult_BYTES+
                        ids_completed.idS_len+2+
                        ids_completed.idU_len+2],
         *ptr=authenticated;

  // nonce
  memcpy(ptr, env->nonce, OPAQUE_NONCE_BYTES);
  ptr+=OPAQUE_NONCE_BYTES;
  // server_public_key
  memcpy(ptr, server_public_key, crypto_scalarmult_BYTES);
  ptr+=crypto_scalarmult_BYTES;
  // server_identity
  uint16_t size = htons(ids_completed.idS_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids_completed.idS,ids_completed.idS_len);
  ptr+=ids_completed.idS_len;
  // client_identity
  size = htons(ids_completed.idU_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids_completed.idU,ids_completed.idU_len);

  opaque_hmacsha512(auth_key,             // key
                    authenticated,        // in
                    sizeof authenticated, // len(in)
                    env->auth_tag);       // out

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(authenticated, sizeof authenticated, "authenticated");
  dump(auth_key, sizeof auth_key, "auth_key");
  dump(env->auth_tag, crypto_auth_hmacsha512_BYTES, "auth_tag");
#endif
  sodium_munlock(auth_key, sizeof auth_key);

#if (defined CFRG_TEST_VEC || defined TRACE)
  dump((uint8_t *)env, OPAQUE_ENVELOPE_BYTES, "envU");
#endif

  return 0;
}

static void personalize_masking_key(const uint8_t *umk, const uint16_t umk_len,
                                    uint8_t masking_key[crypto_hash_sha512_BYTES]) {
  crypto_generichash(masking_key, crypto_hash_sha512_BYTES, // out
        umk, umk_len,                                       // in
        masking_key, crypto_hash_sha512_BYTES);             // key
}

// (StorePwdFile, sid , U, pw): S computes k_s ←_R Z_q , rw := F_k_s (pw),
// p_s ←_R Z_q , p_u ←_R Z_q , P_s := g^p_s , P_u := g^p_u , c ← AuthEnc_rw (p_u, P_u, P_s);
// it records file[sid] := {k_s, p_s, P_s, P_u, c}.
int opaque_Register_core(const uint8_t *pwdU, const uint16_t pwdU_len,
                         const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                         const Opaque_Ids *ids,
                         const uint8_t *unlink_masking_key, const size_t umk_len,
                         uint8_t *_rec,
                         uint8_t export_key[crypto_hash_sha512_BYTES]) {
  Opaque_UserRecord *rec = (Opaque_UserRecord *)_rec;

#ifdef TRACE
  dump(ids->idU, ids->idU_len,"idU ");
  dump(ids->idS, ids->idS_len,"idS ");
#endif

  // k_s ←_R Z_q
  // 1. (kU, _) = KeyGen()
  oprf_KeyGen(rec->kU);

  // rw := F_k_s (pw),
  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU,sizeof rwdU)) {
    return -1;
  }

  if(prf(pwdU, pwdU_len, rec->kU, rwdU)!=0) {
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
#ifdef TRACE
  dump(rwdU, sizeof rwdU, "rwdU");
#endif

  // p_s ←_R Z_q
  if(skS==NULL) {
    randombytes(rec->skS, crypto_scalarmult_SCALARBYTES); // random server secret key
  } else {
    memcpy(rec->skS, skS, crypto_scalarmult_SCALARBYTES);
  }

  // P_s := g^p_s
  uint8_t server_public_key[crypto_scalarmult_BYTES];
  crypto_scalarmult_ristretto255_base(server_public_key, rec->skS);

  uint8_t client_private_key[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(client_private_key,sizeof client_private_key)) {
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  if(0!=skU_from_rwd(rwdU, (uint8_t*) &rec->recU.envelope, client_private_key)) {
    sodium_munlock(client_private_key,sizeof client_private_key);
    sodium_munlock(rwdU, sizeof rwdU);
    return -1;
  }
  // P_u := g^p_u
  crypto_scalarmult_base(rec->recU.client_public_key, client_private_key);
  sodium_munlock(client_private_key,sizeof client_private_key);

  if(0!=create_envelope(rwdU, server_public_key, ids, &rec->recU.envelope, rec->recU.client_public_key, rec->recU.masking_key, export_key)) {
    sodium_munlock(rwdU, sizeof rwdU);
    return -1;
  }
  sodium_munlock(rwdU, sizeof rwdU);
  if(unlink_masking_key!=NULL) personalize_masking_key(unlink_masking_key, umk_len, rec->recU.masking_key);

#ifdef TRACE
  dump(_rec, OPAQUE_USER_RECORD_LEN, "user rec");
#endif
  return 0;
}

int opaque_Register(const uint8_t *pwdU, const uint16_t pwdU_len,
                    const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                    const Opaque_Ids *ids,
                    uint8_t *_rec,
                    uint8_t export_key[crypto_hash_sha512_BYTES]) {
  return opaque_Register_core(pwdU, pwdU_len, skS, ids, NULL, 0, _rec, export_key);
}

int opaque_CreateCredentialRequest_oprf(const uint8_t *pwdU, const uint16_t pwdU_len, uint8_t *_sec, uint8_t *ke1) {
  Opaque_UserSession_Secret *sec = (Opaque_UserSession_Secret*) _sec;
  Opaque_UserSession *pub = (Opaque_UserSession*) ke1;
#ifdef TRACE
  memset(_sec, 0, OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len);
  memset(ke1, 0, OPAQUE_USER_SESSION_PUBLIC_LEN);
#endif

  // 1. (blind, blinded) = Blind(pwdU)
  if(0!=oprf_Blind(pwdU, pwdU_len, sec->blind, pub->blinded)) return -1;
#ifdef TRACE
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len, "sec ");
  dump(ke1,OPAQUE_USER_SESSION_PUBLIC_LEN, "pub ");
#endif
  memcpy(sec->blinded, pub->blinded, crypto_core_ristretto255_BYTES);

  sec->pwdU_len = pwdU_len;
  memcpy(sec->pwdU, pwdU, pwdU_len);

#ifdef TRACE
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len, "sec ");
  dump(ke1,OPAQUE_USER_SESSION_PUBLIC_LEN, "pub ");
#endif

  return 0;
}

int opaque_CreateCredentialRequest_ake(const uint16_t pwdU_len, uint8_t *_sec, uint8_t *ke1) {
  Opaque_UserSession_Secret *sec = (Opaque_UserSession_Secret*) _sec;
  Opaque_UserSession *pub = (Opaque_UserSession*) ke1;

  // x_u ←_R Z_q
#ifndef CFRG_TEST_VEC // if it is defined then so are the values in this block
  const size_t client_keyshare_seed_len=32;
  uint8_t client_keyshare_seed[client_keyshare_seed_len];
  randombytes(client_keyshare_seed, crypto_scalarmult_SCALARBYTES);
#endif

  // nonceU
#ifdef CFRG_TEST_VEC
  memcpy(sec->nonceU, client_nonce, OPAQUE_NONCE_BYTES);
#else
  randombytes(sec->nonceU, OPAQUE_NONCE_BYTES);
#endif
  memcpy(pub->nonceU, sec->nonceU, OPAQUE_NONCE_BYTES);

  deriveKeyPair(client_keyshare_seed, client_keyshare_seed_len, sec->x_u, pub->X_u);

  // keep ke1 for later
  memcpy(sec->ke1, ke1, OPAQUE_USER_SESSION_PUBLIC_LEN);

#ifdef TRACE
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len, "sec ");
  dump(ke1,OPAQUE_USER_SESSION_PUBLIC_LEN, "pub ");
#endif

  return 0;
}

//(UsrSession, sid , ssid , S, pw): U picks r, x_u ←_R Z_q ; sets α := (H^0(pw))^r and
//X_u := g^x_u ; sends α and X_u to S.
// more or less corresponds to CreateCredentialRequest in the irtf draft
int opaque_CreateCredentialRequest(const uint8_t *pwdU, const uint16_t pwdU_len, uint8_t *_sec, uint8_t *ke1) {
  if(0!=opaque_CreateCredentialRequest_oprf(pwdU, pwdU_len, _sec, ke1)) return -1;
  return opaque_CreateCredentialRequest_ake(pwdU_len, _sec, ke1);
}

// more or less corresponds to CreateCredentialResponse in the irtf draft
// 2. (SvrSession, sid , ssid ): On input α from U, S proceeds as follows:
// (a) Checks that α ∈ G^∗ If not, outputs (abort, sid , ssid ) and halts;
// (b) Retrieves file[sid] = {k_s, p_s, P_s, P_u, c};
// (c) Picks x_s ←_R Z_q and computes β := α^k_s and X_s := g^x_s ;
// (d) Computes K := KE(p_s, x_s, P_u, X_u) and SK := f K (0);
// (e) Sends β, X s and c to U;
// (f) Outputs (sid , ssid , SK).
int opaque_CreateCredentialResponse_core(const uint8_t *ke1,
                                         const uint8_t *_rec,
                                         const Opaque_Ids *ids,
                                         const uint8_t *ctx,
                                         const uint16_t ctx_len,
                                         const uint8_t *zero,
                                         const uint8_t *ssid_S, const uint16_t ssid_S_len,
                                         // output
                                         uint8_t *ke2,
                                         uint8_t *sk,
                                         uint8_t authU[crypto_auth_hmacsha512_BYTES]) {
  Opaque_UserSession *pub = (Opaque_UserSession *) ke1;
  Opaque_UserRecord *rec = (Opaque_UserRecord *) _rec;
  Opaque_ServerSession *resp = (Opaque_ServerSession *) ke2;

#ifdef TRACE
  dump(ke1, sizeof(Opaque_UserSession), "session srv pub ");
  dump(_rec, OPAQUE_USER_RECORD_LEN, "session srv rec ");
#endif

  // (a) Checks that α ∈ G^∗ . If not, outputs (abort, sid , ssid ) and halts;
  if(crypto_core_ristretto255_is_valid_point(pub->blinded)!=1) return -1;

  // (b) Retrieves file[sid] = {k_s, p_s, P_s, P_u, c};
  // provided as parameter rec
#ifdef TRACE
  dump(rec->kU, sizeof(rec->kU), "session srv kU ");
  dump(pub->blinded, sizeof(pub->blinded), "session srv blinded ");
#endif

  // computes β := α^k_s
  // 1. Z = Evaluate(DeserializeScalar(credentialFile.kU), request.data)
  if(zero == NULL) {
    if (oprf_Evaluate(rec->kU, pub->blinded, resp->Z) != 0) {
      return -1;
    }
  } else {
    if(ssid_S_len==0 || ssid_S == NULL) {
      return -1;
    }
    uint8_t k[TOPRF_Share_BYTES];
    if(-1==sodium_mlock(k, sizeof k)) return -1;

    uint8_t beta[TOPRF_Part_BYTES];
    k[0]=zero[0];
    memcpy(k+1,rec->kU, TOPRF_Share_BYTES-1);
#ifdef TRACE
    dump(k, sizeof k, "k");
    dump(zero, TOPRF_Share_BYTES, "zero");
    dump(ssid_S, ssid_S_len, "ssid_S");
#endif
    int ret =toprf_3hashtdh(k, zero, pub->blinded, ssid_S, ssid_S_len, beta);
    sodium_munlock(k, sizeof k);
    if(0 != ret) {
      return -1;
    }
    memcpy(resp->Z,beta+1,sizeof(beta)-1);
  }
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(resp->Z, sizeof resp->Z, "EvaluationElement");
#endif

  // 4. masking_nonce = random(Nn)
  // 5. credential_response_pad = Expand(record.masking_key, concat(masking_nonce, "CredentialResponsePad"), Npk + Ne)
  struct {
    uint8_t nonce[32];
    uint8_t dst[21];
  } __attribute((packed)) masking_info = {
      .nonce = {0},
      .dst = "CredentialResponsePad"};
#ifdef CFRG_TEST_VEC
  memcpy(masking_info.nonce, masking_nonce, masking_nonce_len);
#else
  randombytes(masking_info.nonce, sizeof masking_info.nonce);
#endif
  uint8_t response_pad[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  if(-1==sodium_mlock(response_pad, sizeof response_pad)) {
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(response_pad, sizeof response_pad,
                                (const char*) &masking_info, sizeof masking_info,
                                rec->recU.masking_key);
  memcpy(resp->masking_nonce, masking_info.nonce, sizeof masking_info.nonce);

  // recalc server_public_key as we need it for the next step
  uint8_t pkS[crypto_scalarmult_BYTES];
  crypto_scalarmult_ristretto255_base(pkS, rec->skS);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(pkS, sizeof pkS, "server_public_key");
#endif

  memcpy(resp->masked_response, pkS, sizeof pkS);

  // 6. masked_response = xor(credential_response_pad, concat(server_public_key, record.envelope))
  unsigned i;
  for(i=0;i<crypto_scalarmult_BYTES;i++)
    resp->masked_response[i] = response_pad[i] ^ resp->masked_response[i];
  for(;i<crypto_scalarmult_BYTES+sizeof(Opaque_Envelope);i++)
    resp->masked_response[i] = response_pad[i] ^ ((uint8_t*)(&rec->recU.envelope))[i-crypto_scalarmult_BYTES];
  sodium_munlock(response_pad, sizeof response_pad);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(ke2, sizeof (resp->Z) + crypto_scalarmult_BYTES+sizeof(Opaque_Envelope) + sizeof(masking_info.nonce), "resp(z+mn+mr)" );
#endif

  // this is the ake function Response() as per the irtf cfrg draft
  // 1. server_nonce = random(Nn)
  // nonceS
#ifdef CFRG_TEST_VEC
  memcpy(resp->nonceS, server_nonce, OPAQUE_NONCE_BYTES);
#else
  randombytes(resp->nonceS, OPAQUE_NONCE_BYTES);
#endif

  // 2. server_private_keyshare, server_keyshare = GenerateAuthKeyPair()
  // (c) Picks x_s ←_R Z_q
#ifndef CFRG_TEST_VEC
  const size_t server_keyshare_seed_len=32;
  uint8_t server_keyshare_seed[server_keyshare_seed_len];
  randombytes(server_keyshare_seed, crypto_scalarmult_SCALARBYTES);
#endif

  uint8_t x_s[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(x_s,sizeof x_s)) return -1;
  // X_s := g^x_s;
  deriveKeyPair(server_keyshare_seed,server_keyshare_seed_len,x_s,resp->X_s);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(x_s, sizeof(x_s), "session server_private_keyshare (x_s) ");
  dump(resp->X_s, sizeof(resp->X_s), "session server_public_keyshare (X_s)");
#endif

  // 3. Create inner_ke2 ike2 with (credential_response, server_nonce, server_keyshare)
  // should already be all in place

  // 4. preamble = Preamble(client_identity, ke1, server_identity, ike2)
  // mixing in things from the irtf cfrg spec
  char preamble[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state preamble_state;
  calc_preamble(preamble, &preamble_state, rec->recU.client_public_key, pkS, ke1, resp, ctx, ctx_len, (Opaque_Ids*) ids);
  Opaque_Keys keys;
  if(-1==sodium_mlock(&keys,sizeof(keys))) {
    sodium_munlock(x_s,sizeof x_s);
    return -1;
  }

  // (d) Computes K := KE(p_s, x_s, P_u, X_u) and SK := f_K(0);
#ifdef TRACE
  dump(rec->skS,crypto_scalarmult_SCALARBYTES, "rec->skS ");
  dump(x_s,crypto_scalarmult_SCALARBYTES, "x_s ");
  //dump(rec->pkU,crypto_scalarmult_BYTES, "rec->pkU ");
  dump(pub->X_u,crypto_scalarmult_BYTES, "pub->X_u ");
#endif
  // 5. ikm = TripleDHIKM(server_secret, ke1.client_keyshare,
  //                server_private_key, ke1.client_keyshare,
  //                server_secret, client_public_key)
  // 6. Km2, Km3, session_key = DeriveKeys(ikm, preamble)
  if(0!=server_3dh(&keys, rec->skS, x_s, rec->recU.client_public_key, pub->X_u, preamble)) {
    sodium_munlock(x_s, sizeof(x_s));
    sodium_munlock(&keys,sizeof(keys));
    return -1;
  }
  sodium_munlock(x_s, sizeof(x_s));
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(keys.sk, sizeof(keys.sk), "srv sk ");
  dump(keys.km2,OPAQUE_HMAC_SHA512_KEYBYTES,"session srv km2 ");
  dump(keys.km3,OPAQUE_HMAC_SHA512_KEYBYTES,"session srv km3 ");
#endif

  // 7. server_mac = MAC(Km2, Hash(preamble))
  opaque_hmacsha512(keys.km2,
                    (uint8_t*)preamble,                  // in
                    crypto_hash_sha512_BYTES,            // len(in)
                    resp->auth);                         // out
#ifdef TRACE
  dump(resp->auth, sizeof resp->auth, "resp->auth ");
  dump(keys.km2, sizeof keys.km2, "km2 ");
#endif

  // 8. expected_client_mac = MAC(Km3, Hash(concat(preamble, server_mac))
  crypto_hash_sha512_update(&preamble_state, resp->auth, crypto_auth_hmacsha512_BYTES);
  crypto_hash_sha512_final(&preamble_state, (uint8_t *) preamble);
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(resp->auth, crypto_auth_hmacsha512_BYTES, "server mac");
  dump((uint8_t*)preamble, sizeof preamble, "auth preamble");
#endif
  if(NULL!=authU) {
    opaque_hmacsha512(keys.km3,                       // key
                     (uint8_t*)preamble,              // in
                     crypto_hash_sha512_BYTES,        // len(in)
                     authU);                          // out
  }

  memcpy(sk,keys.sk,sizeof(keys.sk));
  sodium_munlock(&keys,sizeof(keys));

#ifdef TRACE
  dump(resp->auth, sizeof(resp->auth), "session srv auth ");
  dump(authU, crypto_auth_hmacsha512_BYTES, "authU");
  dump(ke2, OPAQUE_SERVER_SESSION_LEN, "resp");
#endif

  return 0;
}


int opaque_CreateCredentialResponse(const uint8_t *ke1,
                                    const uint8_t *_rec,
                                    const Opaque_Ids *ids,
                                    const uint8_t *ctx,
                                    const uint16_t ctx_len,
                                    // output
                                    uint8_t *ke2,
                                    uint8_t *sk,
                                    uint8_t authU[crypto_auth_hmacsha512_BYTES]) {
  return opaque_CreateCredentialResponse_core(ke1, _rec, ids, ctx, ctx_len, NULL, NULL, 0, ke2, sk, authU);
}

int opaque_CombineCredentialResponses(const uint8_t t, const uint8_t n,
                                      const uint8_t *indexes,
                                      const uint8_t *ke2s[OPAQUE_SERVER_SESSION_LEN],
                                      uint8_t beta[crypto_scalarmult_ristretto255_BYTES]) {
  if(n<t) return 1; // not enough shares
  if(t<2) return 1; // not a threshold setup
#ifdef TRACE
  dump((uint8_t*)ke2s, n*OPAQUE_SERVER_SESSION_LEN, "ke2s");
#endif
  uint8_t responses[n][TOPRF_Part_BYTES];
  for(int i=0;i<n;i++) {
    if(indexes[i]<1 || indexes[i] > 128) return 1;
    Opaque_ServerSession *ke2 = (Opaque_ServerSession*) ke2s[i];
    responses[i][0]=indexes[i];
    memcpy(&responses[i][1], &ke2->Z, crypto_scalarmult_ristretto255_BYTES);
  }
#ifdef TRACE
  dump((uint8_t*)responses, n*TOPRF_Part_BYTES, "responses");
#endif
  toprf_thresholdmult(t, responses, beta);
#ifdef TRACE
  dump((uint8_t*)beta, crypto_scalarmult_ristretto255_BYTES, "beta");
#endif
  uint8_t tmp[crypto_scalarmult_ristretto255_BYTES];
  for(int i=1;i<=n-t;i++) {
#ifdef TRACE
    dump((uint8_t*)&responses[i], t*TOPRF_Part_BYTES, "shares[%d]", i);
#endif
    toprf_thresholdmult(t, (const uint8_t(*)[TOPRF_Part_BYTES]) &responses[i], tmp);
#ifdef TRACE
    dump(tmp,sizeof tmp, "tmp");
#endif
    if(memcmp(tmp,beta,sizeof tmp)!=0) {
      return 1;
    }
  }

  return 0;
}

// more or less corresponds to RecoverCredentials in the irtf draft
// 3. On β, X_s and c from S, U proceeds as follows:
// (a) Checks that β ∈ G ∗ . If not, outputs (abort, sid , ssid ) and halts;
// (b) Computes rw := H(key, pw|β^1/r );
// (c) Computes AuthDec_rw(c). If the result is ⊥, outputs (abort, sid , ssid ) and halts.
//     Otherwise sets (p_u, P_u, P_s ) := AuthDec_rw (c);
// (d) Computes K := KE(p_u, x_u, P_s, X_s) and SK := f_K(0);
// (e) Outputs (sid, ssid, SK).
int opaque_RecoverCredentials_extBeta(const uint8_t *ke2,
                                      const uint8_t *_sec/*[OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len]*/,
                                      const uint8_t *ctx, const uint16_t ctx_len,
                                      const Opaque_Ids *ids0,
                                      const uint8_t beta[crypto_scalarmult_ristretto255_BYTES],
                                      const uint8_t *unlink_masking_key, const size_t umk_len,
                                      uint8_t *sk,
                                      uint8_t authU[crypto_auth_hmacsha512_BYTES], // aka ke3
                                      uint8_t export_key[crypto_hash_sha512_BYTES]) {

  Opaque_ServerSession *resp = (Opaque_ServerSession *) ke2;
  Opaque_UserSession_Secret *sec = (Opaque_UserSession_Secret *) _sec;

#ifdef TRACE
  dump(sec->pwdU,sec->pwdU_len, "session user finish pwdU ");
  dump(_sec,OPAQUE_USER_SESSION_SECRET_LEN, "session user finish sec ");
  dump(ke2,OPAQUE_SERVER_SESSION_LEN, "session user finish resp ");
  if(beta!=NULL) dump(beta, crypto_scalarmult_ristretto255_BYTES, "beta");
#endif

  // 1. (client_private_key, server_public_key, export_key) =
  //  RecoverCredentials(state.password, state.blind, ke2.CredentialResponse,
  //                     server_identity, client_identity)
  // 1.1. y = Finalize(password, blind, response.data, nil)
  // 1.2. randomized_pwd = Extract("", concat(y, Harden(y, params)))
  uint8_t N[crypto_core_ristretto255_BYTES];
  if(-1==sodium_mlock(N, sizeof N)) return -1;
  // 1. N = Unblind(blind, response.data)
  if(beta==NULL) {
    if(0!=oprf_Unblind(sec->blind, resp->Z, N)) {
      sodium_munlock(N, sizeof N);
      return -1;
    }
  } else {
    if(0!=oprf_Unblind(sec->blind, beta, N)) {
      sodium_munlock(N, sizeof N);
      return -1;
    }
  }
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(N, sizeof N, "unblinded");
#endif

  // rw = H(pw, β^(1/r))
  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU,sizeof rwdU)) {
    sodium_munlock(N, sizeof N);
    return -1;
  }
  // 1.2. y = Finalize(pwdU, N, "OPAQUE01")
  if(0!=finalize(sec->pwdU, sec->pwdU_len, N, rwdU)) {
    sodium_munlock(N, sizeof N);
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  sodium_munlock(N,sizeof N);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(rwdU, sizeof rwdU, "rwdU");
#endif

  // 1.3. masking_key = HKDF-Expand(randomized_pwd, "MaskingKey", Nh)
  const uint8_t masking_key_info[10]="MaskingKey";
  uint8_t masking_key[crypto_hash_sha512_BYTES];
  if(-1==sodium_mlock(masking_key,sizeof masking_key)) {
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(masking_key, crypto_hash_sha512_BYTES,
                                (const char*) masking_key_info, sizeof masking_key_info,
                                rwdU);
  if(unlink_masking_key!=NULL) personalize_masking_key(unlink_masking_key, umk_len, masking_key);

  // 1.4. credential_response_pad = Expand(masking_key,
  //        concat(response.masking_nonce, "CredentialResponsePad"), Npk + Ne)

  // 1.5. credential_response_pad = Expand(record.masking_key, concat(masking_nonce, "CredentialResponsePad"), Npk + Ne)
  struct {
    uint8_t nonce[32];
    uint8_t dst[21];
  } __attribute((packed)) masking_info = {
      .nonce = {0},
      .dst = "CredentialResponsePad"};
  memcpy(masking_info.nonce, resp->masking_nonce, sizeof masking_info.nonce);

  uint8_t response_pad[crypto_scalarmult_BYTES+sizeof(Opaque_Envelope)];
  if(-1==sodium_mlock(response_pad,sizeof response_pad)) {
    sodium_munlock(masking_key,sizeof masking_key);
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(response_pad, sizeof response_pad,
                                (const char*) &masking_info, sizeof masking_info,
                                masking_key);
  sodium_munlock(masking_key,sizeof masking_key);

  // 1.5. concat(server_public_key, envelope) = xor(credential_response_pad,
  //                                            response.masked_response)
  Opaque_Envelope env;
  if(-1==sodium_mlock(&env,sizeof env)) {
    sodium_mlock(response_pad,sizeof response_pad);
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  uint8_t server_public_key[crypto_scalarmult_BYTES], *env_ptr=(uint8_t*) &env;
  unsigned i;
  for(i=0;i<crypto_scalarmult_BYTES;i++)
    server_public_key[i] = response_pad[i] ^ resp->masked_response[i];
  for(;i<crypto_scalarmult_BYTES+sizeof(Opaque_Envelope);i++)
    env_ptr[i-crypto_scalarmult_BYTES] = response_pad[i] ^ resp->masked_response[i];
  sodium_mlock(response_pad,sizeof response_pad);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(server_public_key, sizeof server_public_key, "server_public_key");
  dump(env.nonce, sizeof env.nonce, "env.nonce");
  dump(env.auth_tag, sizeof env.auth_tag, "env.auth_tag");
#endif

  // 1.6. (client_private_key, export_key) =
  //  Recover(randomized_pwd, server_public_key, envelope,
  //                  server_identity, client_identity)

  uint8_t concated[OPAQUE_ENVELOPE_NONCEBYTES+10],
    *label = concated+OPAQUE_ENVELOPE_NONCEBYTES;
  memcpy(concated, env.nonce, OPAQUE_ENVELOPE_NONCEBYTES);

  // 1.6.1. auth_key = Expand(randomized_pwd, concat(envelope.nonce, "AuthKey"), Nh)
  uint8_t auth_key[OPAQUE_HMAC_SHA512_KEYBYTES];
  if(-1==sodium_mlock(auth_key, sizeof auth_key)) {
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  memcpy(label, "AuthKey", 7);
  crypto_kdf_hkdf_sha512_expand(auth_key, sizeof auth_key,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+7,
                                rwdU);

#ifdef TRACE
  dump(auth_key,sizeof auth_key, "auth_key ");
#endif

  if(NULL!=export_key) {
    // 1.6.2. export_key = Expand(randomized_pwd, concat(envelope.nonce, "ExportKey", Nh)
    memcpy(label, "ExportKey", 9);
#if (defined TRACE || defined CFRG_TEST_VEC)
    dump(concated, OPAQUE_ENVELOPE_NONCEBYTES+9, "export_key_info");
#endif
    crypto_kdf_hkdf_sha512_expand(export_key, crypto_hash_sha512_BYTES,
                                  (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+9,
                                  rwdU);
#ifdef TRACE
    dump(export_key,crypto_hash_sha512_BYTES, "export_key ");
#endif
  }

  // 1.6.3. seed = Expand(randomized_pwd, concat(envelope.nonce, "PrivateKey"), Nseed)
  memcpy(label, "PrivateKey", 10);
  uint8_t seed[crypto_core_ristretto255_SCALARBYTES];
  if(-1==sodium_mlock(seed, sizeof seed)) {
    sodium_munlock(auth_key, sizeof auth_key);
    sodium_munlock(rwdU,sizeof rwdU);
    return -1;
  }
  crypto_kdf_hkdf_sha512_expand(seed, crypto_core_ristretto255_SCALARBYTES,
                                (const char*) concated, OPAQUE_ENVELOPE_NONCEBYTES+10,
                                rwdU);
  sodium_munlock(rwdU,sizeof rwdU);

  // 1.6.4. client_private_key, client_public_key = DeriveAuthKeyPair(seed)
  uint8_t client_secret_key[crypto_scalarmult_SCALARBYTES];
  if(-1==sodium_mlock(client_secret_key, sizeof client_secret_key)) {
    sodium_munlock(seed, sizeof seed);
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  uint8_t client_public_key[crypto_scalarmult_BYTES];
  if(0!=deriveKeyPair(seed, sizeof seed, client_secret_key, client_public_key)) {
    sodium_munlock(seed, sizeof seed);
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    sodium_munlock(auth_key, sizeof auth_key);
    return -1;
  }
  sodium_munlock(seed, sizeof seed);
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_secret_key, crypto_scalarmult_SCALARBYTES, "client_secret_key");
#endif
#if (defined CFRG_TEST_VEC || defined TRACE)
  dump(client_public_key, crypto_scalarmult_BYTES, "client_public_key");
#endif

  // 1.6.5. cleartext_creds = CreateCleartextCredentials(server_public_key,
  //                  client_public_key, server_identity, client_identity)

  Opaque_Ids ids;
  fix_ids(client_public_key, server_public_key, ids0, &ids);
  uint8_t authenticated[OPAQUE_NONCE_BYTES+
                        crypto_scalarmult_BYTES+
                        ids.idS_len+2+
                        ids.idU_len+2],
         *ptr=authenticated;

  // nonce
  memcpy(ptr, env.nonce, OPAQUE_NONCE_BYTES);
  ptr+=OPAQUE_NONCE_BYTES;
  // server_public_key
  memcpy(ptr, server_public_key, crypto_scalarmult_BYTES);
  ptr+=crypto_scalarmult_BYTES;
  // server_identity
  uint16_t size = htons(ids.idS_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids.idS,ids.idS_len);
  ptr+=ids.idS_len;
  // client_identity
  size = htons(ids.idU_len);
  memcpy(ptr,(uint8_t*) &size, 2);
  ptr+=2;
  memcpy(ptr,ids.idU,ids.idU_len);

  // 1.6.6. expected_tag = MAC(auth_key, concat(envelope.nonce, cleartext_creds))
  uint8_t auth_tag[crypto_auth_hmacsha512_BYTES];
  opaque_hmacsha512(auth_key,             // key
                    authenticated,        // in
                    sizeof authenticated, // len(in)
                    auth_tag);            // out

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(authenticated, sizeof authenticated, "authenticated");
  dump(auth_key, sizeof auth_key, "auth_key");
  dump(env.auth_tag, crypto_auth_hmacsha512_BYTES, "env auth_tag");
  dump(auth_tag, crypto_hash_sha512_BYTES, "auth tag ");
#endif
  sodium_munlock(auth_key, sizeof auth_key);

  // 1.6.7. If !ct_equal(envelope.auth_tag, expected_tag),
  //   raise KeyRecoveryError
  if(0!=sodium_memcmp(env.auth_tag, auth_tag, sizeof auth_tag)) {
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    return -1;
  }

  // 2. (ke3, session_key) =
  //  ClientFinalize(client_identity, client_private_key, server_identity,

  // 2.2. preamble = Preamble(client_identity, state.ke1, server_identity, ke2.inner_ke2)
  char preamble[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state preamble_state;
  calc_preamble(preamble, &preamble_state, client_public_key, server_public_key, sec->ke1, resp, ctx, ctx_len, &ids);

  Opaque_Keys keys;
  if(-1==sodium_mlock(&keys,sizeof(keys))) {
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    return -1;
  }
  // 2.1. ikm = TripleDHIKM(state.client_secret, ke2.server_keyshare,
  //  state.client_secret, server_public_key, client_private_key, ke2.server_keyshare)
  // 2.3. Km2, Km3, session_key = DeriveKeys(ikm, preamble)
  if(0!=user_3dh(&keys, client_secret_key, sec->x_u, server_public_key, resp->X_s, preamble)) {
    sodium_munlock(client_secret_key, sizeof client_secret_key);
    sodium_munlock(&keys, sizeof(keys));
    return -1;
  }
  sodium_munlock(client_secret_key, sizeof client_secret_key);

  // 2.4. expected_server_mac = MAC(Km2, Hash(preamble))
  uint8_t authS[crypto_auth_hmacsha512_BYTES];
  opaque_hmacsha512(keys.km2,
                    (uint8_t*)preamble,                  // in
                    crypto_hash_sha512_BYTES,            // len(in)
                    authS);                              // out

  // 2.5. If !ct_equal(ke2.server_mac, expected_server_mac),
  //   raise HandshakeError
  if (sodium_memcmp(authS, resp->auth, sizeof authS)!=0) {
    sodium_munlock(&keys, sizeof(keys));
    return -1;
  }

  // 2.6. client_mac = MAC(Km3, Hash(concat(preamble, expected_server_mac))
  crypto_hash_sha512_update(&preamble_state, authS, crypto_auth_hmacsha512_BYTES);
  crypto_hash_sha512_final(&preamble_state, (uint8_t *) preamble);
  if(NULL!=authU) {
    opaque_hmacsha512(keys.km3,                         // key
                      (uint8_t*)preamble,               // in
                      crypto_hash_sha512_BYTES,         // len(in)
                      authU);                           // out
  }

  // 2.7. Create KE3 ke3 with client_mac
  // 2.8. Output (ke3, session_key)
  memcpy(sk,keys.sk,sizeof(keys.sk));

  sodium_munlock(&keys, sizeof(keys));
  return 0;
}

int opaque_RecoverCredentials(const uint8_t *ke2,
                              const uint8_t *_sec/*[OPAQUE_USER_SESSION_SECRET_LEN+pwdU_len]*/,
                              const uint8_t *ctx, const uint16_t ctx_len,
                              const Opaque_Ids *ids0,
                              uint8_t *sk,
                              uint8_t authU[crypto_auth_hmacsha512_BYTES], // aka ke3
                              uint8_t export_key[crypto_hash_sha512_BYTES]) {
  return opaque_RecoverCredentials_extBeta(ke2, _sec, ctx, ctx_len, ids0, NULL, NULL, 0, sk, authU, export_key);
}

// extra function to implement the hmac based auth as defined in the irtf cfrg draft
int opaque_UserAuth(const uint8_t authU0[crypto_auth_hmacsha512_BYTES], const uint8_t authU[crypto_auth_hmacsha512_BYTES]) {
    return sodium_memcmp(authU0, authU, crypto_auth_hmacsha512_BYTES);
}

// variant where the secrets of U never touch S unencrypted

// U computes: blinded PW
// called CreateRegistrationRequest in the irtf cfrg rfc draft
int opaque_CreateRegistrationRequest(const uint8_t *pwdU, const uint16_t pwdU_len, uint8_t *_sec, uint8_t blinded[crypto_core_ristretto255_BYTES]) {
  Opaque_RegisterUserSec *sec = (Opaque_RegisterUserSec *) _sec;
  memcpy(&sec->pwdU, pwdU, pwdU_len);
  sec->pwdU_len = pwdU_len;
  // 1. (blind, blinded) = Blind(pwdU)
  return oprf_Blind(pwdU, pwdU_len, sec->blind, blinded);
}

// initUser: S
// (1) checks α ∈ G^∗ If not, outputs (abort, sid , ssid ) and halts;
// (2) generates k_s ←_R Z_q,
// (3) computes: β := α^k_s,
// (4) finally generates: p_s ←_R Z_q, P_s := g^p_s;
// called CreateRegistrationResponse in the irtf cfrg rfc draft
int opaque_CreateRegistrationResponse_core(const uint8_t blinded[crypto_core_ristretto255_BYTES],
                                           const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                                           const toprf_keygencb keygen,
                                           void* keygen_ctx,
                                           const uint8_t *zero,
                                           const uint8_t *ssid_S, const uint16_t ssid_S_len,
                                           uint8_t *_sec,
                                           uint8_t *_pub) {
  Opaque_RegisterSrvSec *sec = (Opaque_RegisterSrvSec *) _sec;
  Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pub;

  // (a) Checks that α ∈ G^∗ . If not, outputs (abort, sid , ssid ) and halts;
  if(crypto_core_ristretto255_is_valid_point(blinded)!=1) return -1;

  // k_s ←_R Z_q
  // 1. (kU, _) = KeyGen()
  if(keygen==NULL) {
    oprf_KeyGen(sec->kU);
  } else {
    if(0!=keygen(keygen_ctx, sec->kU)) {
      return -1;
    }
  }

  // computes β := α^k_s
  // 2. Z = Evaluate(kU, request.data)
  if (zero == NULL) {
    if (oprf_Evaluate(sec->kU, blinded, pub->Z) != 0) {
      return -1;
    }
  } else {
    if(ssid_S_len==0 || ssid_S == NULL) {
      return -1;
    }
    uint8_t k[TOPRF_Share_BYTES];
    if(-1==sodium_mlock(k, sizeof k)) return -1;
    uint8_t beta[TOPRF_Part_BYTES];
    k[0]=zero[0];
    memcpy(k+1,sec->kU, TOPRF_Share_BYTES-1);
#ifdef TRACE
    dump(k, sizeof k, "k");
    dump(zero, TOPRF_Share_BYTES, "zero");
    dump(ssid_S, ssid_S_len, "ssid_S");
    dump(blinded, crypto_core_ristretto255_BYTES, "blinded");
#endif
    int ret =toprf_3hashtdh(k, zero, blinded, ssid_S, ssid_S_len, beta);
    sodium_munlock(k, sizeof k);
    if(0 != ret) {
      return -1;
    }
    memcpy(pub->Z,beta+1,sizeof(beta)-1);
  }
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(sec->kU, sizeof sec->kU, "kU");
  dump(pub->Z, sizeof pub->Z, "EvaluationElement");
#endif

  if(skS==NULL) {
    randombytes(sec->skS, crypto_scalarmult_SCALARBYTES); // random server long-term key
  } else {
    memcpy(sec->skS, skS, crypto_scalarmult_SCALARBYTES);
  }

#ifdef TRACE
  dump((uint8_t*) sec->skS, sizeof sec->skS, "skS ");
#endif
  // P_s := g^p_s
  crypto_scalarmult_ristretto255_base(pub->pkS, sec->skS);

#ifdef TRACE
  dump((uint8_t*) pub->pkS, sizeof pub->pkS, "pkS ");
#endif

  return 0;
}

int opaque_CreateRegistrationResponse_extKeygen(const uint8_t blinded[crypto_core_ristretto255_BYTES],
                                                const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                                                const toprf_keygencb keygen,
                                                void* keygen_ctx,
                                                uint8_t *_sec,
                                                uint8_t *_pub) {
  return opaque_CreateRegistrationResponse_core(blinded, skS, keygen, keygen_ctx, NULL, NULL, 0, _sec, _pub);
}

int opaque_CreateRegistrationResponse(const uint8_t blinded[crypto_core_ristretto255_BYTES],
                                      const uint8_t skS[crypto_scalarmult_SCALARBYTES],
                                      uint8_t *_sec,
                                      uint8_t *_pub) {
  return opaque_CreateRegistrationResponse_core(blinded, skS, NULL, NULL, NULL, NULL, 0, _sec, _pub);
}

int opaque_CombineRegistrationResponses(const uint8_t t, const uint8_t n,
                                        const uint8_t *_pubs[OPAQUE_REGISTER_PUBLIC_LEN]) {
  if(n<t) return 1; // not enough shares
  if(t<2) return 1; // not a threshold setup
#ifdef TRACE
  dump((uint8_t*)_pubs, n*OPAQUE_REGISTER_PUBLIC_LEN, "pubs");
#endif
  uint8_t responses[n][TOPRF_Part_BYTES];
  for(int i=0;i<n;i++) {
    Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pubs[i];
    responses[i][0]=i+1;
    memcpy(&responses[i][1], &pub->Z, crypto_scalarmult_ristretto255_BYTES);
  }
#ifdef TRACE
  dump((uint8_t*)responses, n*TOPRF_Part_BYTES, "responses");
#endif
  uint8_t beta[crypto_scalarmult_ristretto255_BYTES];
  toprf_thresholdmult(t, responses, beta);
#ifdef TRACE
  dump(beta, crypto_scalarmult_ristretto255_BYTES, "beta");
#endif
  uint8_t tmp[crypto_scalarmult_ristretto255_BYTES];
  for(int i=1;i<=n-t;i++) {
#ifdef TRACE
    dump((uint8_t*)&responses[i], t*TOPRF_Part_BYTES, "shares[%d]", i);
#endif
    toprf_thresholdmult(t, (const uint8_t(*)[TOPRF_Part_BYTES]) &responses[i], tmp);
#ifdef TRACE
    dump(tmp,sizeof tmp, "tmp");
#endif
    if(memcmp(tmp,beta,sizeof tmp)!=0) {
      return 1;
    }
  }
  for(int i=0;i<n;i++) {
    Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pubs[i];
    memcpy(&pub->Z, beta, crypto_scalarmult_ristretto255_BYTES);
  }
  return 0;
}

// user computes:
// (a) Checks that β ∈ G ∗ . If not, outputs (abort, sid , ssid ) and halts;
// (b) Computes rw := H(key, pw | β^1/r );
// (c) p_u ←_R Z_q
// (d) P_u := g^p_u,
// (e) c ← AuthEnc_rw (p_u, P_u, P_s);
// called FinalizeRequest in the irtf cfrg rfc draft
int opaque_FinalizeRequest_core(const uint8_t *_sec/*[OPAQUE_REGISTER_USER_SEC_LEN+pwdU_len]*/,
                                const uint8_t *_pub,
                                const Opaque_Ids *ids,
                                const uint8_t *unlink_masking_key, const size_t umk_len,
                                uint8_t *_rec,
                                uint8_t export_key[crypto_hash_sha512_BYTES]) {

  Opaque_RegisterUserSec *sec = (Opaque_RegisterUserSec *) _sec;
  Opaque_RegisterSrvPub *pub = (Opaque_RegisterSrvPub *) _pub;
  Opaque_RegistrationRecord *rec = (Opaque_RegistrationRecord *) _rec;

  uint8_t N[crypto_core_ristretto255_BYTES];
  if(-1==sodium_mlock(N, sizeof N)) return -1;
  // 1. N = Unblind(blind, response.data)
  if(0!=oprf_Unblind(sec->blind, pub->Z, N)) {
    sodium_munlock(N, sizeof N);
    return -2;
  }
#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(N, sizeof N, "unblinded");
#endif

  uint8_t rwdU[OPAQUE_RWDU_BYTES];
  if(-1==sodium_mlock(rwdU, sizeof rwdU)) {
    sodium_munlock(N, sizeof N);
    return -3;
  }
  // 2. y = Finalize(pwdU, N, "OPAQUE01")
  if(0!=finalize(sec->pwdU, sec->pwdU_len, N, rwdU)) {
    sodium_munlock(N, sizeof N);
    sodium_munlock(rwdU, sizeof(rwdU));
    return -4;
  }
  sodium_munlock(N,sizeof N);

  if(0!=create_envelope(rwdU, pub->pkS, ids, &rec->envelope, rec->client_public_key, rec->masking_key, export_key)) {
    sodium_munlock(rwdU, sizeof rwdU);
    return -5;
  }
  sodium_munlock(rwdU, sizeof rwdU);
  if(unlink_masking_key!=NULL) personalize_masking_key(unlink_masking_key, umk_len, rec->masking_key);

#if (defined TRACE || defined CFRG_TEST_VEC)
  dump(_rec, OPAQUE_REGISTRATION_RECORD_LEN, "record");
#endif

#ifdef TRACE
  dump(_rec, OPAQUE_REGISTRATION_RECORD_LEN, "registration rec ");
#endif

  return 0;
}

int opaque_FinalizeRequest(const uint8_t *_sec/*[OPAQUE_REGISTER_USER_SEC_LEN+pwdU_len]*/,
                           const uint8_t *_pub,
                           const Opaque_Ids *ids,
                           uint8_t *_rec,
                           uint8_t export_key[crypto_hash_sha512_BYTES]) {
   return opaque_FinalizeRequest_core(_sec, _pub, ids, NULL, 0, _rec, export_key);
}

// S records file[sid ] := {k_s, p_s, P_s, P_u, c}.
// called StoreUserRecord in the irtf cfrg rfc draft
void opaque_StoreUserRecord(const uint8_t *_sec, const uint8_t *recU, uint8_t *_rec) {
  Opaque_RegisterSrvSec *sec = (Opaque_RegisterSrvSec *) _sec;
  Opaque_UserRecord *rec = (Opaque_UserRecord *) _rec;

  memcpy(rec->kU, sec->kU, sizeof rec->kU);
  memcpy(rec->skS, sec->skS, crypto_scalarmult_SCALARBYTES);
  memcpy((uint8_t*)&rec->recU, recU, OPAQUE_REGISTRATION_RECORD_LEN);
  //crypto_scalarmult_base(rec->pkS, skS);
#ifdef TRACE
  dump((uint8_t*) rec, OPAQUE_USER_RECORD_LEN, "user rec ");
#endif
}
