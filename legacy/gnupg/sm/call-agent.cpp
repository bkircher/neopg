/* call-agent.c - Divert GPGSM operations to the agent
 * Copyright (C) 2001, 2002, 2003, 2005, 2007,
 *               2008, 2009, 2010 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#include <assuan.h>
#include <gcrypt.h>
#include "../common/asshelp.h"
#include "../common/i18n.h"
#include "../common/membuf.h"
#include "gpgsm.h"
#include "keydb.h" /* fixme: Move this to import.c */
#include "passphrase.h"

static assuan_context_t agent_ctx = NULL;

struct cipher_parm_s {
  ctrl_t ctrl;
  assuan_context_t ctx;
  const unsigned char *ciphertext;
  size_t ciphertextlen;
};

struct genkey_parm_s {
  ctrl_t ctrl;
  assuan_context_t ctx;
  const unsigned char *sexp;
  size_t sexplen;
};

struct learn_parm_s {
  int error;
  ctrl_t ctrl;
  assuan_context_t ctx;
  membuf_t *data;
};

struct import_key_parm_s {
  ctrl_t ctrl;
  assuan_context_t ctx;
  const void *key;
  size_t keylen;
};

struct default_inq_parm_s {
  ctrl_t ctrl;
  assuan_context_t ctx;
};

/* Try to connect to the agent via socket or fork it off and work by
   pipes.  Handle the server's initial greeting */
static int start_agent(ctrl_t ctrl) {
  int rc;

  if (agent_ctx)
    rc = 0; /* fixme: We need a context for each thread or
               serialize the access to the agent (which is
               suitable given that the agent is not MT. */
  else {
    rc = start_new_gpg_agent(&agent_ctx, opt.lc_ctype, opt.lc_messages,
                             opt.autostart, opt.verbose, DBG_IPC);

    if (!opt.autostart && rc == GPG_ERR_NO_AGENT) {
      static int shown;

      if (!shown) {
        shown = 1;
        log_info(_("no gpg-agent running in this session\n"));
      }
    }
  }

  if (!ctrl->agent_seen) {
    ctrl->agent_seen = 1;
  }

  return rc;
}

/* This is the default inquiry callback.  It mainly handles the
   Pinentry notifications.  */
static gpg_error_t default_inq_cb(void *opaque, const char *line) {
  gpg_error_t err = 0;
  struct default_inq_parm_s *parm = (default_inq_parm_s *)opaque;
  ctrl_t ctrl = parm->ctrl;

  if (has_leading_keyword(line, "PINENTRY_LAUNCHED")) {
    err = gpgsm_proxy_pinentry_notify(ctrl, (const unsigned char *)(line));
    if (err)
      log_error(_("failed to proxy %s inquiry to client\n"),
                "PINENTRY_LAUNCHED");
    /* We do not pass errors to avoid breaking other code.  */
  } else if ((has_leading_keyword(line, "PASSPHRASE") ||
              has_leading_keyword(line, "NEW_PASSPHRASE")) &&
             sm_have_static_passphrase()) {
    const char *s = sm_get_static_passphrase();
    err = assuan_send_data(parm->ctx, s, strlen(s));
  } else
    log_error("ignoring gpg-agent inquiry '%s'\n", line);

  return err;
}

/* Call the agent to do a sign operation using the key identified by
   the hex string KEYGRIP. */
int gpgsm_agent_pksign(ctrl_t ctrl, const char *keygrip, const char *desc,
                       unsigned char *digest, size_t digestlen, int digestalgo,
                       unsigned char **r_buf, size_t *r_buflen) {
  int rc, i;
  char *p, line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len;
  struct default_inq_parm_s inq_parm;

  *r_buf = NULL;
  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  if (digestlen * 2 + 50 > DIM(line)) return GPG_ERR_GENERAL;

  rc = assuan_transact(agent_ctx, "RESET", NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  snprintf(line, DIM(line), "SIGKEY %s", keygrip);
  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  if (desc) {
    snprintf(line, DIM(line), "SETKEYDESC %s", desc);
    rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
    if (rc) return rc;
  }

  sprintf(line, "SETHASH %d ", digestalgo);
  p = line + strlen(line);
  for (i = 0; i < digestlen; i++, p += 2) sprintf(p, "%02X", digest[i]);
  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  init_membuf(&data, 1024);
  rc = assuan_transact(agent_ctx, "PKSIGN", put_membuf_cb, &data,
                       default_inq_cb, &inq_parm, NULL, NULL);
  if (rc) {
    xfree(get_membuf(&data, &len));
    return rc;
  }
  *r_buf = (unsigned char *)get_membuf(&data, r_buflen);

  if (!gcry_sexp_canon_len(*r_buf, *r_buflen, NULL, NULL)) {
    xfree(*r_buf);
    *r_buf = NULL;
    return GPG_ERR_INV_VALUE;
  }

  return *r_buf ? 0 : gpg_error_from_syserror();
}

/* Call the scdaemon to do a sign operation using the key identified by
   the hex string KEYID. */
int gpgsm_scd_pksign(ctrl_t ctrl, const char *keyid, const char *desc,
                     unsigned char *digest, size_t digestlen, int digestalgo,
                     unsigned char **r_buf, size_t *r_buflen) {
  int rc, i;
  char *p, line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len;
  const char *hashopt;
  unsigned char *sigbuf;
  size_t sigbuflen;
  struct default_inq_parm_s inq_parm;

  (void)desc;

  *r_buf = NULL;

  switch (digestalgo) {
    case GCRY_MD_SHA1:
      hashopt = "--hash=sha1";
      break;
    case GCRY_MD_RMD160:
      hashopt = "--hash=rmd160";
      break;
    case GCRY_MD_MD5:
      hashopt = "--hash=md5";
      break;
    case GCRY_MD_SHA256:
      hashopt = "--hash=sha256";
      break;
    default:
      return GPG_ERR_DIGEST_ALGO;
  }

  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  if (digestlen * 2 + 50 > DIM(line)) return GPG_ERR_GENERAL;

  p = stpcpy(line, "SCD SETDATA ");
  for (i = 0; i < digestlen; i++, p += 2) sprintf(p, "%02X", digest[i]);
  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  init_membuf(&data, 1024);

  snprintf(line, DIM(line), "SCD PKSIGN %s %s", hashopt, keyid);
  rc = assuan_transact(agent_ctx, line, put_membuf_cb, &data, default_inq_cb,
                       &inq_parm, NULL, NULL);
  if (rc) {
    xfree(get_membuf(&data, &len));
    return rc;
  }
  sigbuf = (unsigned char *)get_membuf(&data, &sigbuflen);

  /* Create an S-expression from it which is formatted like this:
     "(7:sig-val(3:rsa(1:sSIGBUFLEN:SIGBUF)))" Fixme: If a card ever
     creates non-RSA keys we need to change things. */
  *r_buflen = 21 + 11 + sigbuflen + 4;
  p = (char *)xtrymalloc(*r_buflen);
  *r_buf = (unsigned char *)p;
  if (!p) {
    xfree(sigbuf);
    return 0;
  }
  p = stpcpy(p, "(7:sig-val(3:rsa(1:s");
  sprintf(p, "%u:", (unsigned int)sigbuflen);
  p += strlen(p);
  memcpy(p, sigbuf, sigbuflen);
  p += sigbuflen;
  strcpy(p, ")))");
  xfree(sigbuf);

  assert(gcry_sexp_canon_len(*r_buf, *r_buflen, NULL, NULL));
  return 0;
}

/* Handle a CIPHERTEXT inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the end */
static gpg_error_t inq_ciphertext_cb(void *opaque, const char *line) {
  struct cipher_parm_s *parm = (cipher_parm_s *)opaque;
  int rc;

  if (has_leading_keyword(line, "CIPHERTEXT")) {
    assuan_begin_confidential(parm->ctx);
    rc = assuan_send_data(parm->ctx, parm->ciphertext, parm->ciphertextlen);
    assuan_end_confidential(parm->ctx);
  } else {
    struct default_inq_parm_s inq_parm = {parm->ctrl, parm->ctx};
    rc = default_inq_cb(&inq_parm, line);
  }

  return rc;
}

/* Call the agent to do a decrypt operation using the key identified by
   the hex string KEYGRIP. */
int gpgsm_agent_pkdecrypt(ctrl_t ctrl, const char *keygrip, const char *desc,
                          ksba_const_sexp_t ciphertext, char **r_buf,
                          size_t *r_buflen) {
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct cipher_parm_s cipher_parm;
  size_t n, len;
  char *p, *buf, *endp;
  size_t ciphertextlen;

  if (!keygrip || strlen(keygrip) != 40 || !ciphertext || !r_buf || !r_buflen)
    return GPG_ERR_INV_VALUE;
  *r_buf = NULL;

  ciphertextlen = gcry_sexp_canon_len(ciphertext, 0, NULL, NULL);
  if (!ciphertextlen) return GPG_ERR_INV_VALUE;

  rc = start_agent(ctrl);
  if (rc) return rc;

  rc = assuan_transact(agent_ctx, "RESET", NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  assert(DIM(line) >= 50);
  snprintf(line, DIM(line), "SETKEY %s", keygrip);
  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  if (desc) {
    snprintf(line, DIM(line), "SETKEYDESC %s", desc);
    rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
    if (rc) return rc;
  }

  init_membuf(&data, 1024);
  cipher_parm.ctrl = ctrl;
  cipher_parm.ctx = agent_ctx;
  cipher_parm.ciphertext = ciphertext;
  cipher_parm.ciphertextlen = ciphertextlen;
  rc = assuan_transact(agent_ctx, "PKDECRYPT", put_membuf_cb, &data,
                       inq_ciphertext_cb, &cipher_parm, NULL, NULL);
  if (rc) {
    xfree(get_membuf(&data, &len));
    return rc;
  }

  put_membuf(&data, "", 1); /* Make sure it is 0 terminated. */
  buf = (char *)get_membuf(&data, &len);
  if (!buf) return GPG_ERR_ENOMEM;
  assert(len); /* (we forced Nul termination.)  */

  if (*buf == '(') {
    if (len < 13 || memcmp(buf, "(5:value", 8)) /* "(5:valueN:D)\0" */
      return GPG_ERR_INV_SEXP;
    len -= 11;   /* Count only the data of the second part. */
    p = buf + 8; /* Skip leading parenthesis and the value tag. */
  } else {
    /* For compatibility with older gpg-agents handle the old style
       incomplete S-exps. */
    len--; /* Do not count the Nul. */
    p = buf;
  }

  n = strtoul(p, &endp, 10);
  if (!n || *endp != ':') return GPG_ERR_INV_SEXP;
  endp++;
  if (endp - p + n > len)
    return GPG_ERR_INV_SEXP; /* Oops: Inconsistent S-Exp. */

  memmove(buf, endp, n);

  *r_buflen = n;
  *r_buf = buf;
  return 0;
}

/* Handle a KEYPARMS inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the end */
static gpg_error_t inq_genkey_parms(void *opaque, const char *line) {
  struct genkey_parm_s *parm = (genkey_parm_s *)opaque;
  int rc;

  if (has_leading_keyword(line, "KEYPARAM")) {
    rc = assuan_send_data(parm->ctx, parm->sexp, parm->sexplen);
  } else {
    struct default_inq_parm_s inq_parm = {parm->ctrl, parm->ctx};
    rc = default_inq_cb(&inq_parm, line);
  }

  return rc;
}

/* Call the agent to generate a newkey */
int gpgsm_agent_genkey(ctrl_t ctrl, ksba_const_sexp_t keyparms,
                       ksba_sexp_t *r_pubkey) {
  int rc;
  struct genkey_parm_s gk_parm;
  membuf_t data;
  size_t len;
  unsigned char *buf;

  *r_pubkey = NULL;
  rc = start_agent(ctrl);
  if (rc) return rc;

  rc = assuan_transact(agent_ctx, "RESET", NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  init_membuf(&data, 1024);
  gk_parm.ctrl = ctrl;
  gk_parm.ctx = agent_ctx;
  gk_parm.sexp = keyparms;
  gk_parm.sexplen = gcry_sexp_canon_len(keyparms, 0, NULL, NULL);
  if (!gk_parm.sexplen) return GPG_ERR_INV_VALUE;
  rc = assuan_transact(agent_ctx, "GENKEY", put_membuf_cb, &data,
                       inq_genkey_parms, &gk_parm, NULL, NULL);
  if (rc) {
    xfree(get_membuf(&data, &len));
    return rc;
  }
  buf = (unsigned char *)get_membuf(&data, &len);
  if (!buf) return GPG_ERR_ENOMEM;
  if (!gcry_sexp_canon_len(buf, len, NULL, NULL)) {
    xfree(buf);
    return GPG_ERR_INV_SEXP;
  }
  *r_pubkey = buf;
  return 0;
}

/* Call the agent to read the public key part for a given keygrip.  If
   FROMCARD is true, the key is directly read from the current
   smartcard. In this case HEXKEYGRIP should be the keyID
   (e.g. OPENPGP.3). */
int gpgsm_agent_readkey(ctrl_t ctrl, int fromcard, const char *hexkeygrip,
                        ksba_sexp_t *r_pubkey) {
  int rc;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inq_parm;

  *r_pubkey = NULL;
  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  rc = assuan_transact(agent_ctx, "RESET", NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc) return rc;

  snprintf(line, DIM(line), "%sREADKEY %s", fromcard ? "SCD " : "", hexkeygrip);

  init_membuf(&data, 1024);
  rc = assuan_transact(agent_ctx, line, put_membuf_cb, &data, default_inq_cb,
                       &inq_parm, NULL, NULL);
  if (rc) {
    xfree(get_membuf(&data, &len));
    return rc;
  }
  buf = (unsigned char *)get_membuf(&data, &len);
  if (!buf) return GPG_ERR_ENOMEM;
  if (!gcry_sexp_canon_len(buf, len, NULL, NULL)) {
    xfree(buf);
    return GPG_ERR_INV_SEXP;
  }
  *r_pubkey = buf;
  return 0;
}

/* Take the serial number from LINE and return it verbatim in a newly
   allocated string.  We make sure that only hex characters are
   returned. */
static char *store_serialno(const char *line) {
  const char *s;
  char *p;

  for (s = line; hexdigitp(s); s++)
    ;
  p = (char *)xtrymalloc(s + 1 - line);
  if (p) {
    memcpy(p, line, s - line);
    p[s - line] = 0;
  }
  return p;
}

/* Callback for the gpgsm_agent_serialno function.  */
static gpg_error_t scd_serialno_status_cb(void *opaque, const char *line) {
  char **r_serialno = (char **)opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen = 0; *line && !spacep(line); line++, keywordlen++)
    ;
  while (spacep(line)) line++;

  if (keywordlen == 8 && !memcmp(keyword, "SERIALNO", keywordlen)) {
    xfree(*r_serialno);
    *r_serialno = store_serialno(line);
  }

  return 0;
}

/* Call the agent to read the serial number of the current card.  */
int gpgsm_agent_scd_serialno(ctrl_t ctrl, char **r_serialno) {
  int rc;
  char *serialno = NULL;
  struct default_inq_parm_s inq_parm;

  *r_serialno = NULL;
  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  rc = assuan_transact(agent_ctx, "SCD SERIALNO", NULL, NULL, default_inq_cb,
                       &inq_parm, scd_serialno_status_cb, &serialno);
  if (!rc && !serialno) rc = GPG_ERR_INTERNAL;
  if (rc) {
    xfree(serialno);
    return rc;
  }
  *r_serialno = serialno;
  return 0;
}

/* Callback for the gpgsm_agent_serialno function.  */
static gpg_error_t scd_keypairinfo_status_cb(void *opaque, const char *line) {
  strlist_t *listaddr = (string_list **)opaque;
  const char *keyword = line;
  int keywordlen;
  strlist_t sl;
  char *p;

  for (keywordlen = 0; *line && !spacep(line); line++, keywordlen++)
    ;
  while (spacep(line)) line++;

  if (keywordlen == 11 && !memcmp(keyword, "KEYPAIRINFO", keywordlen)) {
    sl = append_to_strlist(listaddr, line);
    p = sl->d;
    /* Make sure that we only have two tokes so that future
       extensions of the format won't change the format expected by
       the caller.  */
    while (*p && !spacep(p)) p++;
    if (*p) {
      while (spacep(p)) p++;
      while (*p && !spacep(p)) p++;
      *p = 0;
    }
  }

  return 0;
}

/* Call the agent to read the keypairinfo lines of the current card.
   The list is returned as a string made up of the keygrip, a space
   and the keyid.  */
int gpgsm_agent_scd_keypairinfo(ctrl_t ctrl, strlist_t *r_list) {
  int rc;
  strlist_t list = NULL;
  struct default_inq_parm_s inq_parm;

  *r_list = NULL;
  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  rc = assuan_transact(agent_ctx, "SCD LEARN --force", NULL, NULL,
                       default_inq_cb, &inq_parm, scd_keypairinfo_status_cb,
                       &list);
  if (!rc && !list) rc = GPG_ERR_NO_DATA;
  if (rc) {
    free_strlist(list);
    return rc;
  }
  *r_list = list;
  return 0;
}

static gpg_error_t istrusted_status_cb(void *opaque, const char *line) {
  struct rootca_flags_s *flags = (rootca_flags_s *)opaque;
  const char *s;

  if ((s = has_leading_keyword(line, "TRUSTLISTFLAG"))) {
    line = s;
    if (has_leading_keyword(line, "relax"))
      flags->relax = 1;
    else if (has_leading_keyword(line, "cm"))
      flags->chain_model = 1;
  }
  return 0;
}

/* Ask the agent whether the certificate is in the list of trusted
   keys.  The certificate is either specified by the CERT object or by
   the fingerprint HEXFPR.  ROOTCA_FLAGS is guaranteed to be cleared
   on error. */
int gpgsm_agent_istrusted(ctrl_t ctrl, ksba_cert_t cert, const char *hexfpr,
                          struct rootca_flags_s *rootca_flags) {
  int rc;
  char line[ASSUAN_LINELENGTH];

  memset(rootca_flags, 0, sizeof *rootca_flags);

  if (cert && hexfpr) return GPG_ERR_INV_ARG;

  rc = start_agent(ctrl);
  if (rc) return rc;

  if (hexfpr) {
    snprintf(line, DIM(line), "ISTRUSTED %s", hexfpr);
  } else {
    char *fpr;

    fpr = gpgsm_get_fingerprint_hexstring(cert, GCRY_MD_SHA1);
    if (!fpr) {
      log_error("error getting the fingerprint\n");
      return GPG_ERR_GENERAL;
    }

    snprintf(line, DIM(line), "ISTRUSTED %s", fpr);
    xfree(fpr);
  }

  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL,
                       istrusted_status_cb, rootca_flags);
  if (!rc) rootca_flags->valid = 1;
  return rc;
}

/* Ask the agent to mark CERT as a trusted Root-CA one */
int gpgsm_agent_marktrusted(ctrl_t ctrl, ksba_cert_t cert) {
  int rc;
  char *fpr, *dn, *dnfmt;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inq_parm;

  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  fpr = gpgsm_get_fingerprint_hexstring(cert, GCRY_MD_SHA1);
  if (!fpr) {
    log_error("error getting the fingerprint\n");
    return GPG_ERR_GENERAL;
  }

  dn = ksba_cert_get_issuer(cert, 0);
  if (!dn) {
    xfree(fpr);
    return GPG_ERR_GENERAL;
  }
  dnfmt = gpgsm_format_name2(dn, 0);
  xfree(dn);
  if (!dnfmt) return gpg_error_from_syserror();
  snprintf(line, DIM(line), "MARKTRUSTED %s S %s", fpr, dnfmt);
  ksba_free(dnfmt);
  xfree(fpr);

  rc = assuan_transact(agent_ctx, line, NULL, NULL, default_inq_cb, &inq_parm,
                       NULL, NULL);
  return rc;
}

/* Ask the agent whether the a corresponding secret key is available
   for the given keygrip */
int gpgsm_agent_havekey(ctrl_t ctrl, const char *hexkeygrip) {
  int rc;
  char line[ASSUAN_LINELENGTH];

  rc = start_agent(ctrl);
  if (rc) return rc;

  if (!hexkeygrip || strlen(hexkeygrip) != 40) return GPG_ERR_INV_VALUE;

  snprintf(line, DIM(line), "HAVEKEY %s", hexkeygrip);

  rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  return rc;
}

static gpg_error_t learn_status_cb(void *opaque, const char *line) {
  struct learn_parm_s *parm = (learn_parm_s *)opaque;
  const char *s;

  /* Pass progress data to the caller.  */
  if ((s = has_leading_keyword(line, "PROGRESS"))) {
    line = s;
    if (parm->ctrl) {
      if (gpgsm_status(parm->ctrl, STATUS_PROGRESS, line))
        return GPG_ERR_ASS_CANCELED;
    }
  }
  return 0;
}

static gpg_error_t learn_cb(void *opaque, const void *buffer, size_t length) {
  struct learn_parm_s *parm = (learn_parm_s *)opaque;
  size_t len;
  char *buf;
  ksba_cert_t cert;
  int rc;

  if (parm->error) return 0;

  if (buffer) {
    put_membuf(parm->data, buffer, length);
    return 0;
  }
  /* END encountered - process what we have */
  buf = (char *)get_membuf(parm->data, &len);
  if (!buf) {
    parm->error = GPG_ERR_ENOMEM;
    return 0;
  }

  if (gpgsm_status(parm->ctrl, STATUS_PROGRESS, "learncard C 0 0"))
    return GPG_ERR_ASS_CANCELED;

  /* FIXME: this should go into import.c */
  rc = ksba_cert_new(&cert);
  if (rc) {
    parm->error = rc;
    return 0;
  }
  rc = ksba_cert_init_from_mem(cert, buf, len);
  if (rc) {
    log_error("failed to parse a certificate: %s\n", gpg_strerror(rc));
    ksba_cert_release(cert);
    parm->error = rc;
    return 0;
  }

  /* We do not store a certifciate with missing issuers as ephemeral
     because we can assume that the --learn-card command has been used
     on purpose.  */
  rc = gpgsm_basic_cert_check(parm->ctrl, cert);
  if (rc && rc != GPG_ERR_MISSING_CERT && rc != GPG_ERR_MISSING_ISSUER_CERT)
    log_error("invalid certificate: %s\n", gpg_strerror(rc));
  else {
    int existed;

    if (!sm_keydb_store_cert(parm->ctrl, cert, 0, &existed)) {
      if (opt.verbose > 1 && existed)
        log_info("certificate already in DB\n");
      else if (opt.verbose && !existed)
        log_info("certificate imported\n");
    }
  }

  ksba_cert_release(cert);
  init_membuf(parm->data, 4096);
  return 0;
}

/* Call the agent to learn about a smartcard */
int gpgsm_agent_learn(ctrl_t ctrl) {
  int rc;
  struct learn_parm_s learn_parm;
  membuf_t data;
  size_t len;

  rc = start_agent(ctrl);
  if (rc) return rc;

  init_membuf(&data, 4096);
  learn_parm.error = 0;
  learn_parm.ctrl = ctrl;
  learn_parm.ctx = agent_ctx;
  learn_parm.data = &data;
  rc = assuan_transact(agent_ctx, "LEARN --send", learn_cb, &learn_parm, NULL,
                       NULL, learn_status_cb, &learn_parm);
  xfree(get_membuf(&data, &len));
  if (rc) return rc;
  return learn_parm.error;
}

/* Ask the agent to change the passphrase of the key identified by
   HEXKEYGRIP. If DESC is not NULL, display instead of the default
   description message. */
int gpgsm_agent_passwd(ctrl_t ctrl, const char *hexkeygrip, const char *desc) {
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inq_parm;

  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  if (!hexkeygrip || strlen(hexkeygrip) != 40) return GPG_ERR_INV_VALUE;

  if (desc) {
    snprintf(line, DIM(line), "SETKEYDESC %s", desc);
    rc = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
    if (rc) return rc;
  }

  snprintf(line, DIM(line), "PASSWD %s", hexkeygrip);

  rc = assuan_transact(agent_ctx, line, NULL, NULL, default_inq_cb, &inq_parm,
                       NULL, NULL);
  return rc;
}

/* Ask the agent to pop up a confirmation dialog with the text DESC
   and an okay and cancel button.  */
gpg_error_t gpgsm_agent_get_confirmation(ctrl_t ctrl, const char *desc) {
  int rc;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inq_parm;

  rc = start_agent(ctrl);
  if (rc) return rc;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  snprintf(line, DIM(line), "GET_CONFIRMATION %s", desc);

  rc = assuan_transact(agent_ctx, line, NULL, NULL, default_inq_cb, &inq_parm,
                       NULL, NULL);
  return rc;
}

/* Return 0 if the agent is alive.  This is useful to make sure that
   an agent has been started. */
gpg_error_t gpgsm_agent_send_nop(ctrl_t ctrl) {
  int rc;

  rc = start_agent(ctrl);
  if (!rc)
    rc = assuan_transact(agent_ctx, "NOP", NULL, NULL, NULL, NULL, NULL, NULL);
  return rc;
}

static gpg_error_t keyinfo_status_cb(void *opaque, const char *line) {
  char **serialno = (char **)opaque;
  const char *s, *s2;

  if ((s = has_leading_keyword(line, "KEYINFO")) && !*serialno) {
    s = strchr(s, ' ');
    if (s && s[1] == 'T' && s[2] == ' ' && s[3]) {
      s += 3;
      s2 = strchr(s, ' ');
      if (s2 > s) {
        *serialno = (char *)xtrymalloc((s2 - s) + 1);
        if (*serialno) {
          memcpy(*serialno, s, s2 - s);
          (*serialno)[s2 - s] = 0;
        }
      }
    }
  }
  return 0;
}

/* Return the serial number for a secret key.  If the returned serial
   number is NULL, the key is not stored on a smartcard.  Caller needs
   to free R_SERIALNO.  */
gpg_error_t gpgsm_agent_keyinfo(ctrl_t ctrl, const char *hexkeygrip,
                                char **r_serialno) {
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  char *serialno = NULL;

  *r_serialno = NULL;

  err = start_agent(ctrl);
  if (err) return err;

  if (!hexkeygrip || strlen(hexkeygrip) != 40) return GPG_ERR_INV_VALUE;

  snprintf(line, DIM(line), "KEYINFO %s", hexkeygrip);

  err = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL,
                        keyinfo_status_cb, &serialno);
  if (!err && serialno) {
    /* Sanity check for bad characters.  */
    if (strpbrk(serialno, ":\n\r")) err = GPG_ERR_INV_VALUE;
  }
  if (err)
    xfree(serialno);
  else
    *r_serialno = serialno;
  return err;
}

/* Ask for the passphrase (this is used for pkcs#12 import/export.  On
   success the caller needs to free the string stored at R_PASSPHRASE.
   On error NULL will be stored at R_PASSPHRASE and an appropriate
   error code returned.  If REPEAT is true the agent tries to get a
   new passphrase (i.e. asks the user to confirm it).  */
gpg_error_t gpgsm_agent_ask_passphrase(ctrl_t ctrl, const char *desc_msg,
                                       int repeat, char **r_passphrase) {
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  char *arg4 = NULL;
  membuf_t data;
  struct default_inq_parm_s inq_parm;

  *r_passphrase = NULL;

  err = start_agent(ctrl);
  if (err) return err;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  if (desc_msg && *desc_msg && !(arg4 = percent_plus_escape(desc_msg)))
    return gpg_error_from_syserror();

  snprintf(line, DIM(line), "GET_PASSPHRASE --data%s -- X X X %s",
           repeat ? " --repeat=1 --check --qualitybar" : "", arg4);
  xfree(arg4);

  init_membuf_secure(&data, 64);
  err = assuan_transact(agent_ctx, line, put_membuf_cb, &data, default_inq_cb,
                        &inq_parm, NULL, NULL);

  if (err)
    xfree(get_membuf(&data, NULL));
  else {
    put_membuf(&data, "", 1);
    *r_passphrase = (char *)get_membuf(&data, NULL);
    if (!*r_passphrase) err = gpg_error_from_syserror();
  }
  return err;
}

/* Handle the inquiry for an IMPORT_KEY command.  */
static gpg_error_t inq_import_key_parms(void *opaque, const char *line) {
  struct import_key_parm_s *parm = (import_key_parm_s *)opaque;
  gpg_error_t err;

  if (has_leading_keyword(line, "KEYDATA")) {
    assuan_begin_confidential(parm->ctx);
    err = assuan_send_data(parm->ctx, parm->key, parm->keylen);
    assuan_end_confidential(parm->ctx);
  } else {
    struct default_inq_parm_s inq_parm = {parm->ctrl, parm->ctx};
    err = default_inq_cb(&inq_parm, line);
  }

  return err;
}

/* Call the agent to import a key into the agent.  */
gpg_error_t gpgsm_agent_import_key(ctrl_t ctrl, const void *key,
                                   size_t keylen) {
  gpg_error_t err;
  struct import_key_parm_s parm;

  err = start_agent(ctrl);
  if (err) return err;

  parm.ctrl = ctrl;
  parm.ctx = agent_ctx;
  parm.key = key;
  parm.keylen = keylen;

  err = assuan_transact(agent_ctx, "IMPORT_KEY", NULL, NULL,
                        inq_import_key_parms, &parm, NULL, NULL);
  return err;
}

/* Receive a secret key from the agent.  KEYGRIP is the hexified
   keygrip, DESC a prompt to be displayed with the agent's passphrase
   question (needs to be plus+percent escaped).  On success the key is
   stored as a canonical S-expression at R_RESULT and R_RESULTLEN. */
gpg_error_t gpgsm_agent_export_key(ctrl_t ctrl, const char *keygrip,
                                   const char *desc, unsigned char **r_result,
                                   size_t *r_resultlen) {
  gpg_error_t err;
  membuf_t data;
  size_t len;
  unsigned char *buf;
  char line[ASSUAN_LINELENGTH];
  struct default_inq_parm_s inq_parm;

  *r_result = NULL;

  err = start_agent(ctrl);
  if (err) return err;
  inq_parm.ctrl = ctrl;
  inq_parm.ctx = agent_ctx;

  if (desc) {
    snprintf(line, DIM(line), "SETKEYDESC %s", desc);
    err = assuan_transact(agent_ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
    if (err) return err;
  }

  snprintf(line, DIM(line), "EXPORT_KEY %s", keygrip);

  init_membuf_secure(&data, 1024);
  err = assuan_transact(agent_ctx, line, put_membuf_cb, &data, default_inq_cb,
                        &inq_parm, NULL, NULL);
  if (err) {
    xfree(get_membuf(&data, &len));
    return err;
  }
  buf = (unsigned char *)get_membuf(&data, &len);
  if (!buf) return gpg_error_from_syserror();
  *r_result = buf;
  *r_resultlen = len;
  return 0;
}
