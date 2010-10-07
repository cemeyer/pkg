#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cdb.h>

#include "util.h"
#include "pkgdb.h"
#include "pkg_compat.h"
#include "pkg_manifest.h"
#include "pkgdb_cache.h"

/* add record formated string */
static int
pkgdb_vadd(struct cdb_make *db, const void *val, size_t vallen, const char *fmt, va_list args)
{
	char key[BUFSIZ];
	size_t len;

	if (db == NULL || key == NULL || val == NULL)
		return (-1);

	len = vsnprintf(key, sizeof(key), fmt, args);

	if (len != strlen(key)) {
		warnx("key too long:");
		vwarnx(fmt, args);
		return (-1);
	}

	/* record the last \0 */
	return (cdb_make_add(db, key, len, val, vallen));
}

/*
static int
pkgdb_add(struct cdb_make *db, const void *val, size_t vallen, const char *fmt, ...)
{
	int ret;
	va_list args;

	va_start(args, fmt);
	ret = pkgdb_vadd(db, val, vallen, fmt, args);
	va_end(args);

	return (ret);
}
*/

/* add record formated string -> string (record the last \0 on value) */
static int
pkgdb_add_string(struct cdb_make *db, const char *val, const char *fmt, ...)
{
	int ret;
	va_list args;

	va_start(args, fmt);
	ret = pkgdb_vadd(db, val, strlen(val)+1, fmt, args);
	va_end(args);

	return (ret);
}

static int
pkgdb_add_int(struct cdb_make *db, const char *key, size_t val)
{
	return cdb_make_add(db, key, strlen(key), &val, sizeof(size_t));
}

static void
pkgdb_cache_rebuild(const char *pkg_dbdir, const char *cache_path)
{
	int fd;
	char tmppath[MAXPATHLEN];
	char mpath[MAXPATHLEN];
	char namever[FILENAME_MAX];
	struct cdb_make cdb;
	size_t idx = 0;
	size_t idep;
	struct pkg_manifest **m = NULL;
	DIR *dir;
	struct dirent *pkg_dir;
	size_t nb_pkg = 0;

	snprintf(tmppath, sizeof(tmppath), "%s/pkgdb.cache-XXXXX", pkg_dbdir);

	if ((fd = mkstemp(tmppath)) == -1)
		return;

	warnx("Rebuilding cache...");
	cdb_make_start(&cdb, fd);

	if ((dir = opendir(pkg_dbdir)) != NULL) {

		/* count potential packages */
		while ((pkg_dir = readdir(dir)) != NULL) {
			if (pkg_dir->d_type == DT_DIR && /* TODO stat(2) for symlinks ? */
					strcmp(pkg_dir->d_name, ".") != 0 &&
					strcmp(pkg_dir->d_name, "..") != 0)
				nb_pkg++;
		}

		if (nb_pkg == 0)
			return;

		rewinddir(dir);
		m = calloc(nb_pkg, sizeof(*m));
		idx = 0;

		while ((pkg_dir = readdir(dir)) != NULL) {
			if (pkg_dir->d_type == DT_DIR && /* TODO stat(2) for symlinks ? */
					strcmp(pkg_dir->d_name, ".") != 0 &&
					strcmp(pkg_dir->d_name, "..") != 0) {

				snprintf(mpath, sizeof(mpath), "%s/%s/+MANIFEST", pkg_dbdir,
					 	 pkg_dir->d_name);

				if ((m[idx] = pkg_manifest_load_file(mpath)) == NULL) {
					warnx("%s not found, converting old +CONTENTS file", mpath);
					if ((m[idx] = pkg_compat_convert_installed(pkg_dbdir, pkg_dir->d_name, mpath))
						== NULL) {
						warnx("error while converting, skipping");
						continue;
					}
				}

				idx++;
			}
			nb_pkg = idx; /* real number of manifest loaded */
		}
		closedir(dir);
	}

	/* sort manifests */
	qsort(m, nb_pkg, sizeof(struct pkg_manifest *), pkg_manifest_cmp);

	for (idx = 0; idx < nb_pkg; idx++) {
		snprintf(namever, sizeof(namever), "%s-%s", pkg_manifest_value(m[idx], "name"),
				 pkg_manifest_value(m[idx], "version"));

		pkgdb_add_int(&cdb, namever, idx);
		pkgdb_add_int(&cdb, pkg_manifest_value(m[idx], "name"), idx);

		pkgdb_add_string(&cdb, namever, PKGDB_NAMEVER, idx);
		pkgdb_add_string(&cdb, pkg_manifest_value(m[idx], "name"), PKGDB_NAME, idx);
		pkgdb_add_string(&cdb, pkg_manifest_value(m[idx], "version"), PKGDB_VERSION, idx);
		pkgdb_add_string(&cdb, pkg_manifest_value(m[idx], "comment"), PKGDB_COMMENT, idx);
		pkgdb_add_string(&cdb, pkg_manifest_value(m[idx], "origin"), PKGDB_ORIGIN, idx);
		pkgdb_add_string(&cdb, pkg_manifest_value(m[idx], "desc"), PKGDB_DESC, idx);

		idep = 0;
		pkg_manifest_dep_init(m[idx]);
		while (pkg_manifest_dep_next(m[idx]) == 0) {
			snprintf(namever, sizeof(namever), "%s-%s", pkg_manifest_dep_name(m[idx]),
					 pkg_manifest_dep_version(m[idx]));
			pkgdb_add_string(&cdb, namever, PKGDB_DEPS, idx, idep);
			idep++;
		}

		pkg_manifest_free(m[idx]);
	}

	if (m != NULL)
		free(m);

	/* record packages len */
	cdb_make_add(&cdb, PKGDB_COUNT, strlen(PKGDB_COUNT), &nb_pkg, sizeof(nb_pkg));
	cdb_make_finish(&cdb);
	close(fd);
	rename(tmppath, cache_path);
	chmod(cache_path, 0644);
}

void
pkgdb_cache_update(struct pkgdb *db)
{
	const char *pkg_dbdir;
	char cache_path[MAXPATHLEN];
	struct stat dir_st, cache_st;
	uid_t uid;

	pkg_dbdir = pkgdb_get_dir();
	uid = getuid();

	if (stat(pkg_dbdir, &dir_st) == -1) {
		if (uid != 0)
			err(EXIT_FAILURE, "%s:", pkg_dbdir);

		if (errno == ENOENT)
			return;
		else
			err(EXIT_FAILURE, "%s:", pkg_dbdir);
	}

	snprintf(cache_path, sizeof(cache_path), "%s/pkgdb.cache", pkg_dbdir);

	errno = 0; /* Reset it in case it is set to ENOENT */
	if (stat(cache_path, &cache_st) == -1 && errno != ENOENT)
		err(EXIT_FAILURE, "%s:", cache_path);

	if (errno == ENOENT || dir_st.st_mtime > cache_st.st_mtime) {
		pkgdb_lock(db, 1);
		pkgdb_cache_rebuild(pkg_dbdir, cache_path);
		pkgdb_unlock(db);
	}
}
