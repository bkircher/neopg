/* delete.c - Delete certificates from the keybox.
 * Copyright (C) 2002, 2009 Free Software Foundation, Inc.
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

#include <gcrypt.h>
#include <ksba.h>
#include "gpgsm.h"

#include "keydb.h"

/* Delete a certificate or an secret key from a key database. */
static int delete_one(ctrl_t ctrl, const char *username) {
  int rc = 0;
  KEYDB_SEARCH_DESC desc;
  KEYDB_HANDLE kh = NULL;
  ksba_cert_t cert = NULL;
  int duplicates = 0;
  int is_ephem = 0;

  rc = classify_user_id(username, &desc, 0);
  if (rc) {
    log_error(_("certificate '%s' not found: %s\n"), username,
              gpg_strerror(rc));
    gpgsm_status2(ctrl, STATUS_DELETE_PROBLEM, "1", NULL);
    goto leave;
  }

  kh = sm_keydb_new();
  if (!kh) {
    log_error("keydb_new failed\n");
    goto leave;
  }

  /* If the key is specified in a unique way, include ephemeral keys
     in the search.  */
  if (desc.mode == KEYDB_SEARCH_MODE_FPR ||
      desc.mode == KEYDB_SEARCH_MODE_FPR20 ||
      desc.mode == KEYDB_SEARCH_MODE_FPR16 ||
      desc.mode == KEYDB_SEARCH_MODE_KEYGRIP) {
    is_ephem = 1;
    sm_keydb_set_ephemeral(kh, 1);
  }

  rc = sm_keydb_search(ctrl, kh, &desc, 1);
  if (!rc) rc = sm_keydb_get_cert(kh, &cert);
  if (!rc && !is_ephem) {
    unsigned char fpr[20];

    gpgsm_get_fingerprint(cert, 0, fpr, NULL);

  next_ambigious:
    rc = sm_keydb_search(ctrl, kh, &desc, 1);
    if (rc == -1)
      rc = 0;
    else if (!rc) {
      ksba_cert_t cert2 = NULL;
      unsigned char fpr2[20];

      /* We ignore all duplicated certificates which might have
         been inserted due to program bugs. */
      if (!sm_keydb_get_cert(kh, &cert2)) {
        gpgsm_get_fingerprint(cert2, 0, fpr2, NULL);
        ksba_cert_release(cert2);
        if (!memcmp(fpr, fpr2, 20)) {
          duplicates++;
          goto next_ambigious;
        }
      }
      rc = GPG_ERR_AMBIGUOUS_NAME;
    }
  }
  if (rc) {
    if (rc == -1) rc = GPG_ERR_NO_PUBKEY;
    log_error(_("certificate '%s' not found: %s\n"), username,
              gpg_strerror(rc));
    gpgsm_status2(ctrl, STATUS_DELETE_PROBLEM, "3", NULL);
    goto leave;
  }

  /* We need to search again to get back to the right position. */
  rc = sm_keydb_lock(kh);
  if (rc) {
    log_error(_("error locking keybox: %s\n"), gpg_strerror(rc));
    goto leave;
  }

  do {
    sm_keydb_search_reset(kh);
    rc = sm_keydb_search(ctrl, kh, &desc, 1);
    if (rc) {
      log_error("problem re-searching certificate: %s\n", gpg_strerror(rc));
      goto leave;
    }

    rc = sm_keydb_delete(kh, duplicates ? 0 : 1);
    if (rc) goto leave;
    if (opt.verbose) {
      if (duplicates)
        log_info(_("duplicated certificate '%s' deleted\n"), username);
      else
        log_info(_("certificate '%s' deleted\n"), username);
    }
  } while (duplicates--);

leave:
  sm_keydb_release(kh);
  ksba_cert_release(cert);
  return rc;
}

/* Delete the certificates specified by NAMES. */
int gpgsm_delete(ctrl_t ctrl, strlist_t names) {
  int rc;

  if (!names) {
    log_error("nothing to delete\n");
    return GPG_ERR_NO_DATA;
  }

  for (; names; names = names->next) {
    rc = delete_one(ctrl, names->d);
    if (rc) {
      log_error(_("deleting certificate \"%s\" failed: %s\n"), names->d,
                gpg_strerror(rc));
      return rc;
    }
  }

  return 0;
}
