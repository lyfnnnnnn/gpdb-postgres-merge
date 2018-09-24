/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Commands to manipulate table spaces
 *
 * Tablespaces in PostgreSQL are designed to allow users to determine
 * where the data file(s) for a given database object reside on the file
 * system.
 *
 * A tablespace represents a directory on the file system. At tablespace
 * creation time, the directory must be empty. To simplify things and
 * remove the possibility of having file name conflicts, we isolate
 * files within a tablespace into database-specific subdirectories.
 *
 * To support file access via the information given in RelFileNode, we
 * maintain a symbolic-link map in $PGDATA/pg_tblspc. The symlinks are
 * named by tablespace OIDs and point to the actual tablespace directories.
 * There is also a per-cluster version directory in each tablespace.
 *
 * In GPDB, the "dbid" of the server is also embedded in the path, so that
 * multiple segments running on the host can use the same directory without
 * clashing with each other. In PostgreSQL, the version string used in the
 * path is in TABLESPACE_VERSION_DIRECTORY constant. In GPDB, use the
 * tablespace_version_directory() function, which appends the dbid,
 * instead.
 *
 * Thus the full path to an arbitrary file is
 *			$PGDATA/pg_tblspc/spcoid/GPDB_MAJORVER_CATVER_db<dbid>/dboid/relfilenode
 * e.g.
 *			$PGDATA/pg_tblspc/20981/GPDB_8.5_201001061_db1/719849/83292814
 *
 *
 * There are two tablespaces created at initdb time: pg_global (for shared
 * tables) and pg_default (for everything else).  For backwards compatibility
 * and to remain functional on platforms without symlinks, these tablespaces
 * are accessed specially: they are respectively
 *			$PGDATA/global/relfilenode
 *			$PGDATA/base/dboid/relfilenode
 *
 * To allow CREATE DATABASE to give a new database a default tablespace
 * that's different from the template database's default, we make the
 * provision that a zero in pg_class.reltablespace means the database's
 * default tablespace.  Without this, CREATE DATABASE would have to go in
 * and munge the system catalogs of the new database.
 *
 *
 * Portions Copyright (c) 2005-2010 Greenplum Inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/standby.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "catalog/heap.h"
#include "catalog/oid_dispatch.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "miscadmin.h"

/* GUC variables */
char	   *default_tablespace = NULL;
char	   *temp_tablespaces = NULL;


static void create_tablespace_directories(const char *location,
							  const Oid tablespaceoid);
static bool destroy_tablespace_directories(Oid tablespaceoid, bool redo);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * If tablespaces are not supported, we still need it in case we have to
 * re-create a database subdirectory (of $PGDATA/base) during WAL replay.
 */
void
TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo)
{
	struct stat st;
	char	   *dir;

	/*
	 * The global tablespace doesn't have per-database subdirectories, so
	 * nothing to do for it.
	 */
	if (spcNode == GLOBALTABLESPACE_OID)
		return;

	Assert(OidIsValid(spcNode));
	Assert(OidIsValid(dbNode));

	dir = GetDatabasePath(dbNode, spcNode);

	if (stat(dir, &st) < 0)
	{
		/* Directory does not exist? */
		if (errno == ENOENT)
		{
			/*
			 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
			 * or TablespaceCreateDbspace is running concurrently.
			 */
			LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

			/*
			 * Recheck to see if someone created the directory while we were
			 * waiting for lock.
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* Directory was created */
			}
			else
			{
				/* Directory creation failed? */
				if (mkdir(dir, S_IRWXU) < 0)
				{
					char	   *parentdir;

					/* Failure other than not exists or not in WAL replay? */
					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 dir)));

					/*
					 * Parent directories are missing during WAL replay, so
					 * continue by creating simple parent directories rather
					 * than a symlink.
					 */

					/* create two parents up if not exist */
					parentdir = pstrdup(dir);
					get_parent_directory(parentdir);
					/* Can't create parent and it doesn't already exist? */
					if (mkdir(parentdir, S_IRWXU) < 0 && errno != EEXIST)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 parentdir)));
					pfree(parentdir);

					/* create one parent up if not exist */
					parentdir = pstrdup(dir);
					get_parent_directory(parentdir);
					/* Can't create parent and it doesn't already exist? */
					if (mkdir(parentdir, S_IRWXU) < 0 && errno != EEXIST)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 parentdir)));
					pfree(parentdir);

					/* Create database directory */
					if (mkdir(dir, S_IRWXU) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 dir)));
				}
			}

			LWLockRelease(TablespaceCreateLock);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else
	{
		/* Is it not a directory? */
		if (!S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" exists but is not a directory",
							dir)));
	}

	pfree(dir);
}

/*
 * Create a table space
 *
 * Only superusers can create a tablespace. This seems a reasonable restriction
 * since we're determining the system layout and, anyway, we probably have
 * root if we're doing this kind of activity
 */
Oid
CreateTableSpace(CreateTableSpaceStmt *stmt)
{
#ifdef HAVE_SYMLINK
	Relation	rel;
	Datum		values[Natts_pg_tablespace];
	bool		nulls[Natts_pg_tablespace];
	HeapTuple	tuple;
	Oid			tablespaceoid;
	char	   *location = NULL;
	Oid			ownerId;
	Datum		newOptions;

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create tablespace \"%s\"",
						stmt->tablespacename),
				 errhint("Must be superuser to create a tablespace.")));

	/* However, the eventual owner of the tablespace need not be */
	if (stmt->owner)
		ownerId = get_role_oid(stmt->owner, false);
	else
		ownerId = GetUserId();

	/* If we have segment-level overrides */
	if (list_length(stmt->options) > 0)
	{
		ListCell   *option;

		foreach(option, stmt->options)
		{
			DefElem	   *defel = (DefElem *) lfirst(option);

			/* Segment content ID specific locations */
			if (strlen(defel->defname) > strlen("content") &&
				strncmp(defel->defname, "content", strlen("content")) == 0)
			{
				int contentId = pg_atoi(defel->defname + strlen("content"), sizeof(int16), 0);

				/*
				 * The master validates the content ids are in [0, segCount)
				 * before dispatching. We can use primary segment count
				 * because the number of primary segments can never shrink and
				 * therefore should not have holes in the content id sequence.
				 */
				if (Gp_role == GP_ROLE_DISPATCH)
				{
					if (contentId < 0 || contentId >= getgpsegmentCount())
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("segment content ID %d does not exist", contentId),
								 errhint("Segment content IDs can be found in gp_segment_configuration table.")));
				}
				else if (contentId == GpIdentity.segindex)
				{
					location = pstrdup(strVal(defel->arg));
					break;
				}
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid segment specification"),
						 errhint("Segment-level locations can be specified using \"contentX <path>\" where X is the content ID.")));
		}
	}

	if (!location)
		location = pstrdup(stmt->location);

	/* Unix-ify the offered path, and strip any trailing slashes */
	canonicalize_path(location);

	/* disallow quotes, else CREATE DATABASE would be at risk */
	if (strchr(location, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("tablespace location cannot contain single quotes")));

	/*
	 * Allowing relative paths seems risky
	 *
	 * this also helps us ensure that location is not empty or whitespace
	 */
	if (!is_absolute_path(location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location must be an absolute path")));

	/*
	 * Check that location isn't too long. Remember that we're going to append
	 * 'PG_XXX/<dboid>/<relid>_<fork>.<nnn>'.  FYI, we never actually
	 * reference the whole path here, but mkdir() uses the first two parts.
	 */
	if (strlen(location) + 1 + strlen(tablespace_version_directory()) + 1 +
	  OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1 + OIDCHARS > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long",
						location)));

	/*
	 * Disallow creation of tablespaces named "pg_xxx"; we reserve this
	 * namespace for system purposes.
	 */
	if (!allowSystemTableMods && IsReservedName(stmt->tablespacename))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"",
						stmt->tablespacename),
		errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * Check that there is no other tablespace by this name.  (The unique
	 * index would catch this anyway, but might as well give a friendlier
	 * message.)
	 */
	if (OidIsValid(get_tablespace_oid(stmt->tablespacename, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						stmt->tablespacename)));

	/*
	 * Insert tuple into pg_tablespace.  The purpose of doing this first is to
	 * lock the proposed tablename against other would-be creators. The
	 * insertion will roll back if we find problems below.
	 */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	MemSet(nulls, false, sizeof(nulls));

	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		ObjectIdGetDatum(ownerId);
	nulls[Anum_pg_tablespace_spcacl - 1] = true;

	/* Generate new proposed spcoptions (text array) */
	newOptions = transformRelOptions((Datum) 0,
									 stmt->options,
									 NULL, NULL, false, false);
	(void) tablespace_reloptions(newOptions, true);
	if (newOptions != (Datum) 0)
		values[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		nulls[Anum_pg_tablespace_spcoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	tablespaceoid = simple_heap_insert(rel, tuple);

	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);

	/* Record dependency on owner */
	recordDependencyOnOwner(TableSpaceRelationId, tablespaceoid, ownerId);

	/* Post creation hook for new tablespace */
	InvokeObjectPostCreateHook(TableSpaceRelationId, tablespaceoid, 0);

	create_tablespace_directories(location, tablespaceoid);

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_create_rec xlrec;
		XLogRecData rdata[2];

		xlrec.ts_id = tablespaceoid;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = offsetof(xl_tblspc_create_rec, ts_path);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = (char *) location;
		rdata[1].len = strlen(location) + 1;
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = NULL;

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_CREATE, rdata);
	}

	/*
	 * Force synchronous commit, to minimize the window between creating the
	 * symlink on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 */
	ForceSyncCommit();

	pfree(location);

	/* We keep the lock on pg_tablespace until commit */
	heap_close(rel, NoLock);

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbDispatchUtilityStatement((Node *) stmt,
									DF_CANCEL_ON_ERROR|
									DF_WITH_SNAPSHOT|
									DF_NEED_TWO_PHASE,
									GetAssignedOidsForDispatch(),
									NULL);

		/* MPP-6929: metadata tracking */
		MetaTrackAddObject(TableSpaceRelationId,
						   tablespaceoid,
						   GetUserId(),
						   "CREATE", "TABLESPACE");
	}

#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif   /* HAVE_SYMLINK */

	return tablespaceoid;
}

/*
 * Drop a table space
 *
 * Be careful to check that the tablespace is empty.
 */
void
DropTableSpace(DropTableSpaceStmt *stmt)
{
#ifdef HAVE_SYMLINK
	char	   *tablespacename = stmt->tablespacename;
	HeapScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	Oid			tablespaceoid;

	/*
	 * Find the target tuple
	 */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("tablespace \"%s\" does not exist, skipping",
							tablespacename)));
			/* XXX I assume I need one or both of these next two calls */
			heap_endscan(scandesc);
			heap_close(rel, NoLock);
		}
		return;
	}

	tablespaceoid = HeapTupleGetOid(tuple);

	/* Must be tablespace owner */
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   tablespacename);

	/* Disallow drop of the standard tablespaces, even by superuser */
	if (tablespaceoid == GLOBALTABLESPACE_OID ||
		tablespaceoid == DEFAULTTABLESPACE_OID)
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE,
					   tablespacename);

	/* DROP hook for the tablespace being removed */
	InvokeObjectDropHook(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Remove the pg_tablespace tuple (this will roll back if we fail below)
	 */
	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);

	/*
	 * Remove any comments or security labels on this tablespace.
	 */
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);
	DeleteSharedSecurityLabel(tablespaceoid, TableSpaceRelationId);

	/*
	 * Remove dependency on owner.
	 */
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid, 0);

	/* MPP-6929: metadata tracking */
	if (Gp_role == GP_ROLE_DISPATCH)
		MetaTrackDropObject(TableSpaceRelationId,
							tablespaceoid);

	/*
	 * Acquire TablespaceCreateLock to ensure that no
	 * MirroredFileSysObj_JustInTimeDbDirCreate is running concurrently.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/*
	 * Try to remove the physical infrastructure.
	 */
	if (!destroy_tablespace_directories(tablespaceoid, false))
	{
		/*
		 * Not all files deleted?  However, there can be lingering empty files
		 * in the directories, left behind by for example DROP TABLE, that
		 * have been scheduled for deletion at next checkpoint (see comments
		 * in mdunlink() for details).  We could just delete them immediately,
		 * but we can't tell them apart from important data files that we
		 * mustn't delete.  So instead, we force a checkpoint which will clean
		 * out any lingering files, and try again.
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
		if (!destroy_tablespace_directories(tablespaceoid, false))
		{
			/* Still not empty, the files must be important then */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));
		}
	}

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_drop_rec xlrec;
		XLogRecData rdata[1];

		xlrec.ts_id = tablespaceoid;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_tblspc_drop_rec);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = NULL;

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP, rdata);
	}

	/*
	 * Note: because we checked that the tablespace was empty, there should be
	 * no need to worry about flushing shared buffers or free space map
	 * entries for relations in the tablespace.
	 */

	/*
	 * Force synchronous commit, to minimize the window between removing the
	 * files on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 */
	ForceSyncCommit();

	/*
	 * Allow MirroredFileSysObj_JustInTimeDbDirCreate again.
	 */
	LWLockRelease(TablespaceCreateLock);

	/* We keep the lock on the row in pg_tablespace until commit */
	heap_close(rel, NoLock);

	/*
	 * If we are the QD, dispatch this DROP command to all the QEs
	 */
	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbDispatchUtilityStatement((Node *) stmt,
									DF_CANCEL_ON_ERROR|
									DF_WITH_SNAPSHOT|
									DF_NEED_TWO_PHASE,
									NIL,
									NULL);
	}

#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif   /* HAVE_SYMLINK */
}


/*
 * create_tablespace_directories
 *
 *	Attempt to create filesystem infrastructure linking $PGDATA/pg_tblspc/
 *	to the specified directory
 */
static void
create_tablespace_directories(const char *location, const Oid tablespaceoid)
{
	char	   *linkloc;
	char	   *location_with_version_dir;
	struct stat st;

	linkloc = psprintf("pg_tblspc/%u", tablespaceoid);
	location_with_version_dir = psprintf("%s/%s", location,
										tablespace_version_directory());

	/*
	 * Attempt to coerce target directory to safe permissions.  If this fails,
	 * it doesn't exist or has the wrong owner.
	 */
	if (chmod(location, S_IRWXU) != 0)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FILE),
					 errmsg("directory \"%s\" does not exist", location),
					 InRecovery ? errhint("Create this directory for the tablespace before "
										  "restarting the server.") : 0));
		else
			ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set permissions on directory \"%s\": %m",
						location)));
	}

	if (InRecovery)
	{
		/*
		 * Our theory for replaying a CREATE is to forcibly drop the target
		 * subdirectory if present, and then recreate it. This may be more
		 * work than needed, but it is simple to implement.
		 */
		if (stat(location_with_version_dir, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (!rmtree(location_with_version_dir, true))
				/* If this failed, mkdir() below is going to error. */
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								location_with_version_dir)));
		}
	}

	/*
	 * The creation of the version directory prevents more than one tablespace
	 * in a single location.
	 */
	if (mkdir(location_with_version_dir, S_IRWXU) < 0)
	{
		if (errno == EEXIST)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("directory \"%s\" already in use as a tablespace",
							location_with_version_dir)));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
				  errmsg("could not create directory \"%s\": %m",
						 location_with_version_dir)));
	}

	/*
	 * In recovery, remove old symlink, in case it points to the wrong place.
	 *
	 * On Windows, junction points act like directories so we must be able to
	 * apply rmdir; in general it seems best to make this code work like the
	 * symlink removal code in destroy_tablespace_directories, except that
	 * failure to remove is always an ERROR.
	 */
	if (InRecovery)
	{
		if (lstat(linkloc, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (rmdir(linkloc) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove directory \"%s\": %m",
								linkloc)));
		}
		else
		{
			if (unlink(linkloc) < 0 && errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove symbolic link \"%s\": %m",
								linkloc)));
		}
	}

	/*
	 * Create the symlink under PGDATA
	 */
	if (symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	pfree(linkloc);
	pfree(location_with_version_dir);
}


/*
 * destroy_tablespace_directories
 *
 * Attempt to remove filesystem infrastructure for the tablespace.
 *
 * 'redo' indicates we are redoing a drop from XLOG; in that case we should
 * not throw an ERROR for problems, just LOG them.  The worst consequence of
 * not removing files here would be failure to release some disk space, which
 * does not justify throwing an error that would require manual intervention
 * to get the database running again.
 *
 * Returns TRUE if successful, FALSE if some subdirectory is not empty
 */
static bool
destroy_tablespace_directories(Oid tablespaceoid, bool redo)
{
	char	   *linkloc;
	char	   *linkloc_with_version_dir;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;
	struct stat st;

	linkloc_with_version_dir = psprintf("pg_tblspc/%u/%s", tablespaceoid,
										tablespace_version_directory());

	/*
	 * Check if the tablespace still contains any files.  We try to rmdir each
	 * per-database directory we find in it.  rmdir failure implies there are
	 * still files in that subdirectory, so give up.  (We do not have to worry
	 * about undoing any already completed rmdirs, since the next attempt to
	 * use the tablespace from that database will simply recreate the
	 * subdirectory via MirroredFileSysObj_JustInTimeDbDirCreate.)
	 *
	 * Since we hold TablespaceCreateLock, no one else should be creating any
	 * fresh subdirectories in parallel. It is possible that new files are
	 * being created within subdirectories, though, so the rmdir call could
	 * fail.  Worst consequence is a less friendly error message.
	 *
	 * If redo is true then ENOENT is a likely outcome here, and we allow it
	 * to pass without comment.  In normal operation we still allow it, but
	 * with a warning.  This is because even though ProcessUtility disallows
	 * DROP TABLESPACE in a transaction block, it's possible that a previous
	 * DROP failed and rolled back after removing the tablespace directories
	 * and/or symlink.  We want to allow a new DROP attempt to succeed at
	 * removing the catalog entries (and symlink if still present), so we
	 * should not give a hard error here.
	 */
	dirdesc = AllocateDir(linkloc_with_version_dir);
	if (dirdesc == NULL)
	{
		if (errno == ENOENT)
		{
			if (!redo)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m",
								linkloc_with_version_dir)));
			/* The symlink might still exist, so go try to remove it */
			goto remove_symlink;
		}
		else if (redo)
		{
			/* in redo, just log other types of error */
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							linkloc_with_version_dir)));
			pfree(linkloc_with_version_dir);
			return false;
		}
		/* else let ReadDir report the error */
	}

	while ((de = ReadDir(dirdesc, linkloc_with_version_dir)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		subfile = psprintf("%s/%s", linkloc_with_version_dir, de->d_name);

		/* This check is just to deliver a friendlier error message */
		if (!redo && !directory_is_empty(subfile))
		{
			FreeDir(dirdesc);
			pfree(subfile);
			pfree(linkloc_with_version_dir);
			return false;
		}

		/* remove empty directory */
		if (rmdir(subfile) < 0)
			ereport(redo ? LOG : ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}

	FreeDir(dirdesc);

	/* remove version directory */
	if (rmdir(linkloc_with_version_dir) < 0)
	{
		ereport(redo ? LOG : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m",
						linkloc_with_version_dir)));
		pfree(linkloc_with_version_dir);
		return false;
	}

	/*
	 * Try to remove the symlink.  We must however deal with the possibility
	 * that it's a directory instead of a symlink --- this could happen during
	 * WAL replay (see TablespaceCreateDbspace), and it is also the case on
	 * Windows where junction points lstat() as directories.
	 *
	 * Note: in the redo case, we'll return true if this final step fails;
	 * there's no point in retrying it.  Also, ENOENT should provoke no more
	 * than a warning.
	 */
remove_symlink:
	linkloc = pstrdup(linkloc_with_version_dir);
	get_parent_directory(linkloc);
	if (lstat(linkloc, &st) == 0 && S_ISDIR(st.st_mode))
	{
		if (rmdir(linkloc) < 0)
			ereport(redo ? LOG : ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
	}
	else
	{
		if (unlink(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
		}
	}

	pfree(linkloc_with_version_dir);
	pfree(linkloc);

	return true;
}


/*
 * Check if a directory is empty.
 *
 * This probably belongs somewhere else, but not sure where...
 */
bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);

	while ((de = ReadDir(dirdesc, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		FreeDir(dirdesc);
		return false;
	}

	FreeDir(dirdesc);
	return true;
}


/*
 * Rename a tablespace
 */
Oid
RenameTableSpace(const char *oldname, const char *newname)
{
	Oid			tspId;
	Relation	rel;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	Oid			tablespaceoid;
	HeapTuple	tup;
	HeapTuple	newtuple;
	Form_pg_tablespace newform;

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = heap_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						oldname)));

	tspId = HeapTupleGetOid(tup);
	newtuple = heap_copytuple(tup);
	newform = (Form_pg_tablespace) GETSTRUCT(newtuple);

	heap_endscan(scan);

	/* Must be owner */
	tablespaceoid = HeapTupleGetOid(newtuple);
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE, oldname);

	/* Validate new name */
	if (!allowSystemTableMods && IsReservedName(newname))
	{
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"", newname),
				 errdetail("The prefix \"%s\" is reserved for system tablespaces.",
						   GetReservedPrefix(newname))));
	}

	/* Make sure the new name doesn't exist */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = heap_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						newname)));

	heap_endscan(scan);

	/* OK, update the entry */
	namestrcpy(&(newform->spcname), newname);

	simple_heap_update(rel, &newtuple->t_self, newtuple);
	CatalogUpdateIndexes(rel, newtuple);

	/* MPP-6929: metadata tracking */
	if (Gp_role == GP_ROLE_DISPATCH)
		MetaTrackUpdObject(TableSpaceRelationId,
						   tablespaceoid,
						   GetUserId(),
						   "ALTER", "RENAME"
				);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tspId, 0);

	heap_close(rel, NoLock);

	return tspId;
}

/*
 * Alter table space options
 */
Oid
AlterTableSpaceOptions(AlterTableSpaceOptionsStmt *stmt)
{
	Relation	rel;
	ScanKeyData entry[1];
	HeapScanDesc scandesc;
	HeapTuple	tup;
	Oid			tablespaceoid;
	Datum		datum;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_tablespace];
	bool		isnull;
	bool		repl_null[Natts_pg_tablespace];
	bool		repl_repl[Natts_pg_tablespace];
	HeapTuple	newtuple;

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->tablespacename));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						stmt->tablespacename)));

	tablespaceoid = HeapTupleGetOid(tup);

	/* Must be owner of the existing object */
	if (!pg_tablespace_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   stmt->tablespacename);

	/* Generate new proposed spcoptions (text array) */
	datum = heap_getattr(tup, Anum_pg_tablespace_spcoptions,
						 RelationGetDescr(rel), &isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 stmt->options, NULL, NULL, false,
									 stmt->isReset);
	(void) tablespace_reloptions(newOptions, true);

	/* Build new tuple. */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_tablespace_spcoptions - 1] = true;
	repl_repl[Anum_pg_tablespace_spcoptions - 1] = true;
	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val,
								 repl_null, repl_repl);

	/* Update system catalog. */
	simple_heap_update(rel, &newtuple->t_self, newtuple);
	CatalogUpdateIndexes(rel, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, HeapTupleGetOid(tup), 0);

	heap_freetuple(newtuple);

	/* Conclude heap scan. */
	heap_endscan(scandesc);
	heap_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * Alter table space move
 *
 * Allows a user to move all of their objects in a given tablespace in the
 * current database to another tablespace. Only objects which the user is
 * considered to be an owner of are moved and the user must have CREATE rights
 * on the new tablespace. These checks should mean that ALTER TABLE will never
 * fail due to permissions, but note that permissions will also be checked at
 * that level. Objects can be ALL, TABLES, INDEXES, or MATERIALIZED VIEWS.
 *
 * All to-be-moved objects are locked first. If NOWAIT is specified and the
 * lock can't be acquired then we ereport(ERROR).
 */
Oid
AlterTableSpaceMove(AlterTableSpaceMoveStmt *stmt)
{
	List	   *relations = NIL;
	ListCell   *l;
	ScanKeyData key[1];
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Oid			orig_tablespaceoid;
	Oid			new_tablespaceoid;
	List	   *role_oids = roleNamesToIds(stmt->roles);

	/* Ensure we were not asked to move something we can't */
	if (!stmt->move_all && stmt->objtype != OBJECT_TABLE &&
		stmt->objtype != OBJECT_INDEX && stmt->objtype != OBJECT_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only tables, indexes, and materialized views exist in tablespaces")));

	/* Get the orig and new tablespace OIDs */
	orig_tablespaceoid = get_tablespace_oid(stmt->orig_tablespacename, false);
	new_tablespaceoid = get_tablespace_oid(stmt->new_tablespacename, false);

	/* Can't move shared relations in to or out of pg_global */
	/* This is also checked by ATExecSetTableSpace, but nice to stop earlier */
	if (orig_tablespaceoid == GLOBALTABLESPACE_OID ||
		new_tablespaceoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global tablespace")));

	/*
	 * Must have CREATE rights on the new tablespace, unless it is the
	 * database default tablespace (which all users implicitly have CREATE
	 * rights on).
	 */
	if (OidIsValid(new_tablespaceoid) && new_tablespaceoid != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(new_tablespaceoid, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(new_tablespaceoid));
	}

	/*
	 * Now that the checks are done, check if we should set either to
	 * InvalidOid because it is our database's default tablespace.
	 */
	if (orig_tablespaceoid == MyDatabaseTableSpace)
		orig_tablespaceoid = InvalidOid;

	if (new_tablespaceoid == MyDatabaseTableSpace)
		new_tablespaceoid = InvalidOid;

	/* no-op */
	if (orig_tablespaceoid == new_tablespaceoid)
		return new_tablespaceoid;

	/*
	 * Walk the list of objects in the tablespace and move them. This will
	 * only find objects in our database, of course.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_reltablespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(orig_tablespaceoid));

	rel = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			relOid = HeapTupleGetOid(tuple);
		Form_pg_class relForm;

		relForm = (Form_pg_class) GETSTRUCT(tuple);

		/*
		 * Do not move objects in pg_catalog as part of this, if an admin
		 * really wishes to do so, they can issue the individual ALTER
		 * commands directly.
		 *
		 * Also, explicitly avoid any shared tables, temp tables, or TOAST
		 * (TOAST will be moved with the main table).
		 */
		if (IsSystemNamespace(relForm->relnamespace) || relForm->relisshared ||
			isAnyTempNamespace(relForm->relnamespace) ||
			relForm->relnamespace == PG_TOAST_NAMESPACE)
			continue;

		/* Only consider objects which live in tablespaces */
		if (relForm->relkind != RELKIND_RELATION &&
			relForm->relkind != RELKIND_INDEX &&
			relForm->relkind != RELKIND_MATVIEW)
			continue;

		/* Check if we were asked to only move a certain type of object */
		if (!stmt->move_all &&
			((stmt->objtype == OBJECT_TABLE &&
			  relForm->relkind != RELKIND_RELATION) ||
			 (stmt->objtype == OBJECT_INDEX &&
			  relForm->relkind != RELKIND_INDEX) ||
			 (stmt->objtype == OBJECT_MATVIEW &&
			  relForm->relkind != RELKIND_MATVIEW)))
			continue;

		/* Check if we are only moving objects owned by certain roles */
		if (role_oids != NIL && !list_member_oid(role_oids, relForm->relowner))
			continue;

		/*
		 * Handle permissions-checking here since we are locking the tables
		 * and also to avoid doing a bunch of work only to fail part-way. Note
		 * that permissions will also be checked by AlterTableInternal().
		 *
		 * Caller must be considered an owner on the table to move it.
		 */
		if (!pg_class_ownercheck(relOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   NameStr(relForm->relname));

		if (stmt->nowait &&
			!ConditionalLockRelationOid(relOid, AccessExclusiveLock))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
			   errmsg("aborting due to \"%s\".\"%s\" --- lock not available",
					  get_namespace_name(relForm->relnamespace),
					  NameStr(relForm->relname))));
		else
			LockRelationOid(relOid, AccessExclusiveLock);

		/* Add to our list of objects to move */
		relations = lappend_oid(relations, relOid);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (relations == NIL)
		ereport(NOTICE,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("no matching relations in tablespace \"%s\" found",
					orig_tablespaceoid == InvalidOid ? "(database default)" :
						get_tablespace_name(orig_tablespaceoid))));

	/* Everything is locked, loop through and move all of the relations. */
	foreach(l, relations)
	{
		List	   *cmds = NIL;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_SetTableSpace;
		cmd->name = stmt->new_tablespacename;

		cmds = lappend(cmds, cmd);

		AlterTableInternal(lfirst_oid(l), cmds, false);
	}

	return new_tablespaceoid;
}

/*
 * Routines for handling the GUC variable 'default_tablespace'.
 */

/*
 * Returns true if tablespace exists, false otherwise
 */
static bool
check_tablespace(const char *tablespacename)
{
	bool		result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace. We use a heapscan here even though there is an
	 * index on name, on the theory that pg_tablespace will usually have just
	 * a few entries and so an indexed lookup is a waste of effort.
	 */
	rel = heap_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* If nothing matches then the tablespace doesn't exist */
	if (HeapTupleIsValid(tuple))
		result = true;
	else
		result = false;

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return result;
}

/* check_hook: validate new default_tablespace */
bool
check_default_tablespace(char **newval, void **extra, GucSource source)
{
	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the name.  Must accept the value on faith.
	 */
	if (IsTransactionState())
	{
		/*
		 * get_tablespace_oid cannot be used because it acquires lock hence
		 * ends up allocating xid (maybe in reader gang too) instead
		 * check_tablespace is used.
		 */
		if (**newval != '\0' &&
			!check_tablespace(*newval))
		{
			/*
			 * When source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 */
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tablespace \"%s\" does not exist",
								*newval)));
			}
			else
			{
				GUC_check_errdetail("Tablespace \"%s\" does not exist.",
									*newval);
				return false;
			}
		}
	}

	return true;
}

/*
 * GetDefaultTablespace -- get the OID of the current default tablespace
 *
 * Temporary objects have different default tablespaces, hence the
 * relpersistence parameter must be specified.
 *
 * May return InvalidOid to indicate "use the database's default tablespace".
 *
 * Note that caller is expected to check appropriate permissions for any
 * result other than InvalidOid.
 *
 * This exists to hide (and possibly optimize the use of) the
 * default_tablespace GUC variable.
 */
Oid
GetDefaultTablespace(char relpersistence)
{
	Oid			result;

	/* The temp-table case is handled elsewhere */
	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		PrepareTempTablespaces();
		return GetNextTempTableSpace();
	}

	/* Fast path for default_tablespace == "" */
	if (default_tablespace == NULL || default_tablespace[0] == '\0')
		return InvalidOid;

	/*
	 * It is tempting to cache this lookup for more speed, but then we would
	 * fail to detect the case where the tablespace was dropped since the GUC
	 * variable was set.  Note also that we don't complain if the value fails
	 * to refer to an existing tablespace; we just silently return InvalidOid,
	 * causing the new object to be created in the database's tablespace.
	 */
	result = get_tablespace_oid(default_tablespace, true);

	/*
	 * Allow explicit specification of database's default tablespace in
	 * default_tablespace without triggering permissions checks.
	 */
	if (result == MyDatabaseTableSpace)
		result = InvalidOid;
	return result;
}


/*
 * Routines for handling the GUC variable 'temp_tablespaces'.
 */

typedef struct
{
	int			numSpcs;
	Oid			tblSpcs[1];		/* VARIABLE LENGTH ARRAY */
} temp_tablespaces_extra;

/* check_hook: validate new temp_tablespaces */
bool
check_temp_tablespaces(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	List	   *namelist;

	/* Need a modifiable copy of string */
	rawname = pstrdup(*newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}

	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the individual names.  Must accept the list on faith.
	 * Fortunately, there's then also no need to pass the data to fd.c.
	 */
	if (IsTransactionState())
	{
		temp_tablespaces_extra *myextra;
		Oid		   *tblSpcs;
		int			numSpcs;
		ListCell   *l;

		/* temporary workspace until we are done verifying the list */
		tblSpcs = (Oid *) palloc(list_length(namelist) * sizeof(Oid));
		numSpcs = 0;
		foreach(l, namelist)
		{
			char	   *curname = (char *) lfirst(l);
			Oid			curoid;
			AclResult	aclresult;

			/* Allow an empty string (signifying database default) */
			if (curname[0] == '\0')
			{
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/*
			 * In an interactive SET command, we ereport for bad info.  When
			 * source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 */
			curoid = get_tablespace_oid(curname, source <= PGC_S_TEST);
			if (curoid == InvalidOid)
			{
				if (source == PGC_S_TEST)
					ereport(NOTICE,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace \"%s\" does not exist",
									curname)));
				continue;
			}

			/*
			 * Allow explicit specification of database's default tablespace
			 * in temp_tablespaces without triggering permissions checks.
			 */
			if (curoid == MyDatabaseTableSpace)
			{
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/* Check permissions, similarly complaining only if interactive */
			aclresult = pg_tablespace_aclcheck(curoid, GetUserId(),
											   ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
			{
				if (source >= PGC_S_INTERACTIVE)
					aclcheck_error(aclresult, ACL_KIND_TABLESPACE, curname);
				continue;
			}

			tblSpcs[numSpcs++] = curoid;
		}

		/* Now prepare an "extra" struct for assign_temp_tablespaces */
		myextra = malloc(offsetof(temp_tablespaces_extra, tblSpcs) +
						 numSpcs * sizeof(Oid));
		if (!myextra)
			return false;
		myextra->numSpcs = numSpcs;
		memcpy(myextra->tblSpcs, tblSpcs, numSpcs * sizeof(Oid));
		*extra = (void *) myextra;

		pfree(tblSpcs);
	}

	pfree(rawname);
	list_free(namelist);

	return true;
}

/* assign_hook: do extra actions as needed */
void
assign_temp_tablespaces(const char *newval, void *extra)
{
	temp_tablespaces_extra *myextra = (temp_tablespaces_extra *) extra;

	/*
	 * If check_temp_tablespaces was executed inside a transaction, then pass
	 * the list it made to fd.c.  Otherwise, clear fd.c's list; we must be
	 * still outside a transaction, or else restoring during transaction exit,
	 * and in either case we can just let the next PrepareTempTablespaces call
	 * make things sane.
	 */
	if (myextra)
		SetTempTablespaces(myextra->tblSpcs, myextra->numSpcs);
	else
		SetTempTablespaces(NULL, 0);
}

/*
 * PrepareTempTablespaces -- prepare to use temp tablespaces
 *
 * If we have not already done so in the current transaction, parse the
 * temp_tablespaces GUC variable and tell fd.c which tablespace(s) to use
 * for temp files.
 */
void
PrepareTempTablespaces(void)
{
	char	   *rawname;
	List	   *namelist;
	Oid		   *tblSpcs;
	int			numSpcs;
	ListCell   *l;

	/* No work if already done in current transaction */
	if (TempTablespacesAreSet())
		return;

	/*
	 * Can't do catalog access unless within a transaction.  This is just a
	 * safety check in case this function is called by low-level code that
	 * could conceivably execute outside a transaction.  Note that in such a
	 * scenario, fd.c will fall back to using the current database's default
	 * tablespace, which should always be OK.
	 */
	if (!IsTransactionState())
		return;

	/* Need a modifiable copy of string */
	rawname = pstrdup(temp_tablespaces);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		SetTempTablespaces(NULL, 0);
		pfree(rawname);
		list_free(namelist);
		return;
	}

	/* Store tablespace OIDs in an array in TopTransactionContext */
	tblSpcs = (Oid *) MemoryContextAlloc(TopTransactionContext,
										 list_length(namelist) * sizeof(Oid));
	numSpcs = 0;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			curoid;
		AclResult	aclresult;

		/* Allow an empty string (signifying database default) */
		if (curname[0] == '\0')
		{
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Else verify that name is a valid tablespace name */
		curoid = get_tablespace_oid(curname, true);
		if (curoid == InvalidOid)
		{
			/* Skip any bad list elements */
			continue;
		}

		/*
		 * Allow explicit specification of database's default tablespace in
		 * temp_tablespaces without triggering permissions checks.
		 */
		if (curoid == MyDatabaseTableSpace)
		{
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Check permissions similarly */
		aclresult = pg_tablespace_aclcheck(curoid, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			continue;

		tblSpcs[numSpcs++] = curoid;
	}

	SetTempTablespaces(tblSpcs, numSpcs);

	pfree(rawname);
	list_free(namelist);
}


/*
 * get_tablespace_oid - given a tablespace name, look up the OID
 *
 * If missing_ok is false, throw an error if tablespace name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_tablespace_oid(const char *tablespacename, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on name, on the theory that pg_tablespace will usually have just
	 * a few entries and so an indexed lookup is a waste of effort.
	 */
	rel = heap_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* If nothing matches then the tablespace doesn't exist */
	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	/*
	 * Anything that needs to lookup a tablespace name must need a lock
	 * on the tablespace for the duration of its transaction, otherwise
	 * there is nothing preventing it from being dropped.
	 */
	if (OidIsValid(result))
	{
		Buffer			buffer = InvalidBuffer;
		HTSU_Result		lockTest;
		HeapUpdateFailureData hufd;

		/*
		 * Unfortunately locking of objects other than relations doesn't
		 * really work, the work around is to lock the tuple in pg_tablespace
		 * to prevent drops from getting the exclusive lock they need.
		 */
		lockTest = heap_lock_tuple(rel, tuple,
								   GetCurrentCommandId(true),
								   LockTupleKeyShare, LockTupleWait,
								   false,  &buffer, &hufd);
		ReleaseBuffer(buffer);
		switch (lockTest)
		{
			case HeapTupleMayBeUpdated:
				break;  /* Got the Lock */

			case HeapTupleSelfUpdated:
				Assert(false); /* Shouldn't ever occur */
				/* fallthrough */

			case HeapTupleBeingUpdated:
				Assert(false);  /* Not possible with LockTupleWait */
				/* fallthrough */

			case HeapTupleUpdated:
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access to tablespace %s due to concurrent update",
								tablespacename)));

			default:
				elog(ERROR, "unrecognized heap_lock_tuple_status: %u", lockTest);
		}
	}

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						tablespacename)));

	return result;
}

/*
 * get_tablespace_name - given a tablespace OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such tablespace.
 */
char *
get_tablespace_name(Oid spc_oid)
{
	char	   *result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on oid, on the theory that pg_tablespace will usually have just a
	 * few entries and so an indexed lookup is a waste of effort.
	 */
	rel = heap_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = heap_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return result;
}


/*
 * TABLESPACE resource manager's routines
 */
void
tblspc_redo(XLogRecPtr beginLoc, XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in tblspc records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) XLogRecGetData(record);
		char	   *location = xlrec->ts_path;

		create_tablespace_directories(location, xlrec->ts_id);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

		/*
		 * If we issued a WAL record for a drop tablespace it implies that
		 * there were no files in it at all when the DROP was done. That means
		 * that no permanent objects can exist in it at this point.
		 *
		 * It is possible for standby users to be using this tablespace as a
		 * location for their temporary files, so if we fail to remove all
		 * files then do conflict processing and try again, if currently
		 * enabled.
		 *
		 * Other possible reasons for failure include bollixed file
		 * permissions on a standby server when they were okay on the primary,
		 * etc etc. There's not much we can do about that, so just remove what
		 * we can and press on.
		 */
		if (!destroy_tablespace_directories(xlrec->ts_id, true))
		{
			ResolveRecoveryConflictWithTablespace(xlrec->ts_id);

			/*
			 * If we did recovery processing then hopefully the backends who
			 * wrote temp files should have cleaned up and exited by now.  So
			 * retry before complaining.  If we fail again, this is just a LOG
			 * condition, because it's not worth throwing an ERROR for (as
			 * that would crash the database and require manual intervention
			 * before we could get past this WAL record on restart).
			 */
			if (!destroy_tablespace_directories(xlrec->ts_id, true))
				ereport(LOG,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("directories for tablespace %u could not be removed",
						xlrec->ts_id),
						 errhint("You can remove the directories manually if necessary.")));
		}
	}
	else
		elog(PANIC, "tblspc_redo: unknown op code %u", info);
}
