/*
   Unix SMB/CIFS implementation.
   SMB torture tester - mangling test
   Copyright (C) Andrew Tridgell 2002
   Copyright (C) David Mulder 2019

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "system/filesys.h"
#include "system/dir.h"
#include <tdb.h>
#include "../lib/util/util_tdb.h"
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"
#include "torture/util.h"
#include "torture/smb2/proto.h"

#undef strcasecmp

static TDB_CONTEXT *tdb;

#define NAME_LENGTH 20

static unsigned int total, collisions, failures;

static bool test_one(struct torture_context *tctx, struct smb2_tree *tree,
		     const char *name)
{
	struct smb2_handle fnum;
	const char *shortname;
	const char *name2;
	NTSTATUS status;
	TDB_DATA data;
	struct smb2_create io = {0};

	total++;

	io.in.fname = name;
	io.in.desired_access = SEC_FILE_READ_DATA | SEC_FILE_WRITE_DATA |
			       SEC_FILE_EXECUTE;
	io.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
			     NTCREATEX_SHARE_ACCESS_WRITE |
			     NTCREATEX_SHARE_ACCESS_DELETE;
	io.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	status = smb2_create(tree, tree, &io);
	if (!NT_STATUS_IS_OK(status)) {
		torture_comment(tctx, "open of %s failed (%s)\n", name,
				nt_errstr(status));
		return false;
	}
	fnum = io.out.file.handle;

	status = smb2_util_close(tree, fnum);
	if (NT_STATUS_IS_ERR(status)) {
		torture_comment(tctx, "close of %s failed (%s)\n", name,
				nt_errstr(status));
		return false;
	}

	/* get the short name */
	status = smb2_qpathinfo_alt_name(tctx, tree, name, &shortname);
	if (!NT_STATUS_IS_OK(status)) {
		torture_comment(tctx, "query altname of %s failed (%s)\n",
				name, nt_errstr(status));
		return false;
	}

	name2 = talloc_asprintf(tctx, "mangle_test\\%s", shortname);
	status = smb2_util_unlink(tree, name2);
	if (NT_STATUS_IS_ERR(status)) {
		torture_comment(tctx, "unlink of %s  (%s) failed (%s)\n",
		       name2, name, nt_errstr(status));
		return false;
	}

	/* recreate by short name */
	io = (struct smb2_create){0};
	io.in.fname = name2;
	io.in.desired_access = SEC_FILE_READ_DATA | SEC_FILE_WRITE_DATA |
			       SEC_FILE_EXECUTE;
	io.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
			     NTCREATEX_SHARE_ACCESS_WRITE |
			     NTCREATEX_SHARE_ACCESS_DELETE;
	io.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	status = smb2_create(tree, tree, &io);
	if (!NT_STATUS_IS_OK(status)) {
		torture_comment(tctx, "open2 of %s failed (%s)\n", name2,
				nt_errstr(status));
		return false;
	}
	fnum = io.out.file.handle;

	status = smb2_util_close(tree, fnum);
	if (NT_STATUS_IS_ERR(status)) {
		torture_comment(tctx, "close of %s failed (%s)\n", name,
				nt_errstr(status));
		return false;
	}

	/* and unlink by long name */
	status = smb2_util_unlink(tree, name);
	if (NT_STATUS_IS_ERR(status)) {
		torture_comment(tctx, "unlink2 of %s  (%s) failed (%s)\n",
				name, name2, nt_errstr(status));
		failures++;
		smb2_util_unlink(tree, name2);
		return true;
	}

	/* see if the short name is already in the tdb */
	data = tdb_fetch_bystring(tdb, shortname);
	if (data.dptr) {
		/* maybe its a duplicate long name? */
		if (strcasecmp(name, (const char *)data.dptr) != 0) {
			/* we have a collision */
			collisions++;
			torture_comment(tctx, "Collision between %s and %s"
					"   ->  %s  (coll/tot: %u/%u)\n",
					name, data.dptr, shortname, collisions,
					total);
		}
		free(data.dptr);
	} else {
		TDB_DATA namedata;
		/* store it for later */
		namedata.dptr = discard_const_p(uint8_t, name);
		namedata.dsize = strlen(name)+1;
		tdb_store_bystring(tdb, shortname, namedata, TDB_REPLACE);
	}

	return true;
}


static char *gen_name(struct torture_context *tctx)
{
	const char *chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-$~...";
	unsigned int max_idx = strlen(chars);
	unsigned int len;
	int i;
	char *p = NULL;
	char *name = NULL;

	name = talloc_strdup(tctx, "mangle_test\\");
	if (!name) {
		return NULL;
	}

	len = 1 + random() % NAME_LENGTH;

	name = talloc_realloc(tctx, name, char, strlen(name) + len + 6);
	if (!name) {
		return NULL;
	}
	p = name + strlen(name);

	for (i=0;i<len;i++) {
		p[i] = chars[random() % max_idx];
	}

	p[i] = 0;

	if (ISDOT(p) || ISDOTDOT(p)) {
		p[0] = '_';
	}

	/* have a high probability of a common lead char */
	if (random() % 2 == 0) {
		p[0] = 'A';
	}

	/* and a medium probability of a common lead string */
	if ((len > 5) && (random() % 10 == 0)) {
		strlcpy(p, "ABCDE", 6);
	}

	/* and a high probability of a good extension length */
	if (random() % 2 == 0) {
		char *s = strrchr(p, '.');
		if (s) {
			s[4] = 0;
		}
	}

	return name;
}


static bool torture_smb2_mangle(struct torture_context *torture,
				struct smb2_tree *tree)
{
	extern int torture_numops;
	int i;
	bool ok;
	NTSTATUS status;

	/* we will use an internal tdb to store the names we have used */
	tdb = tdb_open(NULL, 100000, TDB_INTERNAL, 0, 0);
	torture_assert(torture, tdb, "ERROR: Failed to open tdb\n");

	ok = smb2_util_setup_dir(torture, tree, "mangle_test");
	torture_assert(torture, ok, "smb2_util_setup_dir failed\n");

	for (i=0;i<torture_numops;i++) {
		char *name;

		name = gen_name(torture);
		torture_assert(torture, name, "Name allocation failed\n");

		ok = test_one(torture, tree, name);
		torture_assert(torture, ok, talloc_asprintf(torture,
			       "Mangle names failed with %s", name));
		if (total && total % 100 == 0) {
			if (torture_setting_bool(torture, "progress", true)) {
				torture_comment(torture,
				       "collisions %u/%u  - %.2f%%   (%u failures)\r",
				       collisions, total, (100.0*collisions) / total, failures);
			}
		}
	}

	smb2_util_unlink(tree, "mangle_test\\*");
	status = smb2_util_rmdir(tree, "mangle_test");
	torture_assert_ntstatus_ok(torture, status,
				   "ERROR: Failed to remove directory\n");

	torture_comment(torture,
			"\nTotal collisions %u/%u  - %.2f%%   (%u failures)\n",
			collisions, total, (100.0*collisions) / total, failures);

	return (failures == 0);
}

static bool test_mangled_mask(struct torture_context *tctx,
			      struct smb2_tree *tree)
{
	bool ret = true;
	const char *dname = "single_find_with_mangled_name";
	const char *fname = "single_find_with_mangled_name\\verylongfilename";
	const char *shortname = NULL;
	NTSTATUS status;
	struct smb2_handle dh = {{0}};
	struct smb2_handle fh = {{0}};
	struct smb2_find f;
	union smb_search_data *d;
	unsigned count, i;

	torture_comment(tctx, "Checking find with mangled search mask\n");

	smb2_deltree(tree, dname);

	/* Create dname and fname */
	status = torture_smb2_testdir(tree, dname, &dh);
	torture_assert_ntstatus_ok_goto(tctx, status, ret, done,
					"torture_smb2_testdir failed");

	status = torture_smb2_testfile(tree, fname, &fh);
	smb2_util_close(tree, fh);

	smb2_util_close(tree, dh);

	/* Update the directory handle, that readdir() can see the testfile */
	status = torture_smb2_testdir(tree, dname, &dh);
	torture_assert_ntstatus_ok_goto(tctx, status, ret, done,
					"torture_smb2_testdir failed");

	ZERO_STRUCT(f);
	f.in.file.handle	= dh;
	f.in.pattern		= "*";
	f.in.max_response_size	= 0x1000;
	f.in.level              = SMB2_FIND_BOTH_DIRECTORY_INFO;

	status = smb2_find_level(tree, tree, &f, &count, &d);
	torture_assert_ntstatus_ok_goto(tctx, status, ret, done,
					"smb2_find_level failed\n");
	smb2_util_close(tree, dh);
	ZERO_STRUCT(dh);

	for (i = 0; i < count; i++) {
		const char *found = d[i].both_directory_info.name.s;

		if (!strcmp(found, ".") || !strcmp(found, "..")) {
			continue;
		}

		torture_assert_str_equal_goto(tctx, found, "verylongfilename", ret, done,
					      "Bad filename\n");
		shortname = d[i].both_directory_info.short_name.s;
		break;
	}

	torture_assert_not_null_goto(tctx, shortname, ret, done,
				     "shortname is NULL\n");

	torture_comment(tctx, "Got shortname: %s\n", shortname);

	status = torture_smb2_testdir(tree, dname, &dh);

	ZERO_STRUCT(f);
	f.in.file.handle	= dh;
	f.in.continue_flags	= SMB2_CONTINUE_FLAG_SINGLE;
	f.in.pattern		= shortname;
	f.in.max_response_size	= 0x1000;
	f.in.level              = SMB2_FIND_BOTH_DIRECTORY_INFO;

	status = smb2_find_level(tree, tree, &f, &count, &d);
	torture_assert_ntstatus_ok_goto(tctx, status, ret, done,
					"smb2_find_level failed\n");
	smb2_util_close(tree, dh);
	ZERO_STRUCT(dh);

done:
	if (!smb2_util_handle_empty(fh)) {
		smb2_util_close(tree, fh);
	}
	if (!smb2_util_handle_empty(dh)) {
		smb2_util_close(tree, dh);
	}
	smb2_deltree(tree, dname);
	return ret;

}

struct torture_suite *torture_smb2_name_mangling_init(TALLOC_CTX *ctx)
{
	struct torture_suite *suite = NULL;

	suite = torture_suite_create(ctx, "name-mangling");
	suite->description = talloc_strdup(suite, "SMB2 name mangling tests");

	torture_suite_add_1smb2_test(suite, "mangle", torture_smb2_mangle);
	torture_suite_add_1smb2_test(suite, "mangled-mask", test_mangled_mask);
	return suite;
}
