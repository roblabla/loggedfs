/*****************************************************************************
 * Author:   Remi Flament <remipouak at gmail dot com>
 *****************************************************************************
 * Copyright (c) 2005 - 2016, Remi Flament
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */


#ifdef linux
  /* For pread()/pwrite() */
  #define _X_SOURCE 500
#endif

#include <sqlite3.h>
#include <pthread.h>
#include <curl/curl.h>
#include <assert.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#ifndef __APPLE__
  #include <sys/statfs.h>
#endif
#ifdef HAVE_SETXATTR
  #include <sys/xattr.h>
#endif
#include <stdarg.h>
#include <getopt.h>
#include <sys/time.h>
#include <pwd.h>
#include <grp.h>
#include "Config.h"

#define PUSHARG(ARG) \
assert(out->fuseArgc < MaxFuseArgs); \
out->fuseArgv[out->fuseArgc++] = ARG

using namespace std;

static Config config;
static int savefd;

const int MaxFuseArgs = 32;
struct LoggedFS_Args
{
    char* mountPoint; // where the users read files
    char* configFilename;
    bool isDaemon; // true == spawn in background, log to syslog
    const char *fuseArgv[MaxFuseArgs];
    int fuseArgc;
};

struct env {
    sqlite3 *db;
    pthread_cond_t *cond_var;
    pthread_mutex_t *m;
};

static struct env env;

static LoggedFS_Args *loggedfsArgs = new LoggedFS_Args;


static bool isAbsolutePath( const char *fileName )
{
    if(fileName && fileName[0] != '\0' && fileName[0] == '/')
        return true;
    else
        return false;
}


static char* getAbsolutePath(const char *path)
{
    char *realPath=new char[strlen(path)+strlen(loggedfsArgs->mountPoint)+1];

    strcpy(realPath,loggedfsArgs->mountPoint);
    if (realPath[strlen(realPath)-1]=='/')
        realPath[strlen(realPath)-1]='\0';
    strcat(realPath,path);
    return realPath;

}

static char* getRelativePath(const char* path)
{
    char* rPath=new char[strlen(path)+2];

    strcpy(rPath,".");
    strcat(rPath,path);

    return rPath;
}

/*
 * Returns the name of the process which accessed the file system.
 */
static char* getcallername()
{
    char filename[100];
    sprintf(filename,"/proc/%d/cmdline",fuse_get_context()->pid);
    FILE * proc=fopen(filename,"rt");
    char cmdline[256]="";
    fread(cmdline,sizeof(cmdline),1,proc);
    fclose(proc);
    return strdup(cmdline);
}

int upload_single(void *data, int col_nbr, char **cols, char **col_name)
{
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "http://bbfs.seed-up.org/data");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    for (int i = 0; i < col_nbr; i++)
    {
        if (strcmp(col_name[i], "str") == 0)
        {
            printf("Add to postfields: %s\n", cols[i]);
            char *json;
            asprintf(&json, "{\"str\": \"%s\"}", cols[i]);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(json));
            break;
        }
    }
    int res = curl_easy_perform(curl);
    int http_code = 0;

    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res != CURLE_OK)
        dprintf(2, "%s:%d: ERROR with curl\n", __FILE__, __LINE__);
    else if (http_code >= 400)
        dprintf(2, "%s:%d: ERROR http code: %d\n", __FILE__, __LINE__, http_code);
    else
        (void)1; // TODO: Data is sended so set uploadted to 1
    curl_easy_cleanup(curl);
    return (0);
}

void *upload_thread(void *data)
{
    pthread_mutex_lock(env.m);
    while (true)
    {
        // SELECT * FROM db WHERE uploaded=false
        // upload
        // update
        // upload
        // update
        // upload
        // update ?
        //
        //
        // Or maybe I can actually try to be slightly smarter and batch upload ?
        // Also, we should probably uniquely identify the machine. Need to store
        // a UUID somewhere (preferably in the sqlite db) that says who this is.
        //
        // This way, I can just bulk send with the UUID, and the merge should be
        // easy.
        // TODO: Proper error handling
        sqlite3_exec(env.db, "SELECT * FROM logs WHERE uploaded=0", upload_single, NULL, NULL);
        pthread_cond_wait(env.cond_var, env.m);
    }
    pthread_mutex_unlock(env.m);
}

void send_log(sqlite3 *db, pthread_cond_t *cond_var, char *str) {
    sqlite3_stmt *stmt;

    // First, store it in sql
    sqlite3_prepare_v2(db, "INSERT INTO logs (str, uploaded) VALUES (?, 0)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, str, -1, &free);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        printf("ERROR inserting data: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_finalize(stmt);

    // Then, send it to the upload thread
    pthread_cond_signal(cond_var);
}

static void loggedfs_log(const char* path,const char* action,const int returncode,const char *format,...)
{
    const char *retname;
    char *mybuf;
    if (returncode >= 0)
        retname = "SUCCESS";
    else
        retname = "FAILURE";
    if (config.shouldLog(path,fuse_get_context()->uid,action,retname))
    {
        va_list args;
        char buf[1024];
        va_start(args,format);
        memset(buf,0,1024);
        vsprintf(buf,format,args);
        strcat(buf," {%s} [ pid = %d %s uid = %d ]");
        asprintf(&mybuf, buf,retname, fuse_get_context()->pid,config.isPrintProcessNameEnabled()?getcallername():"", fuse_get_context()->uid);
        send_log(env.db, env.cond_var, mybuf);
        va_end(args);
    }
}

static void* loggedFS_init(struct fuse_conn_info* info)
{
     fchdir(savefd);
     close(savefd);
     return NULL;
}


static int loggedFS_getattr(const char *path, struct stat *stbuf)
{
    int res;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = lstat(path, stbuf);
    loggedfs_log(aPath,"getattr",res,"getattr %s",aPath);
    if(res == -1)
        return -errno;

    return 0;
}


static int loggedFS_access(const char *path, int mask)
{
    int res;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = access(path, mask);
    loggedfs_log(aPath,"access",res,"access %s",aPath);
    if (res == -1)
        return -errno;

    return 0;
}



static int loggedFS_readlink(const char *path, char *buf, size_t size)
{
    int res;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = readlink(path, buf, size - 1);
    loggedfs_log(aPath,"readlink",res,"readlink %s",aPath);
    if(res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}

static int loggedFS_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi)
{
    DIR *dp;
    struct dirent *de;
    int res;

    (void) offset;
    (void) fi;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);


    dp = opendir(path);
    if(dp == NULL) {
        res = -errno;
        loggedfs_log(aPath,"readdir",-1,"readdir %s",aPath);
        return res;
    }

    while((de = readdir(dp)) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        if (filler(buf, de->d_name, &st, 0))
            break;
    }

    closedir(dp);
    loggedfs_log(aPath,"readdir",0,"readdir %s",aPath);
    return 0;
}

static int loggedFS_mknod(const char *path, mode_t mode, dev_t rdev)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);


    if (S_ISREG(mode)) {
        res = open(path, O_CREAT | O_EXCL | O_WRONLY, mode);
	loggedfs_log(aPath,"mknod",res,"mknod %s %o S_IFREG (normal file creation)",aPath, mode);
        if (res >= 0)
            res = close(res);
    } else if (S_ISFIFO(mode))
	{
        res = mkfifo(path, mode);
	loggedfs_log(aPath,"mkfifo",res,"mkfifo %s %o S_IFFIFO (fifo creation)",aPath, mode);
	}
    else
	{
        res = mknod(path, mode, rdev);
	if (S_ISCHR(mode))
		{
	        loggedfs_log(aPath,"mknod",res,"mknod %s %o (character device creation)",aPath, mode);
		}
	/*else if (S_IFBLK(mode))
		{
		loggedfs_log(aPath,"mknod",res,"mknod %s %o (block device creation)",aPath, mode);
		}*/
	else loggedfs_log(aPath,"mknod",res,"mknod %s %o",aPath, mode);
	}

	if (res == -1)
	        return -errno;
	else
		{
		lchown(path, fuse_get_context()->uid, fuse_get_context()->gid);
		}

    return 0;
}

static int loggedFS_mkdir(const char *path, mode_t mode)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = mkdir(path, mode);
    loggedfs_log(getRelativePath(aPath),"mkdir",res,"mkdir %s %o",aPath, mode);
    if(res == -1)
        return -errno;
    else
        lchown(path, fuse_get_context()->uid, fuse_get_context()->gid);
    return 0;
}

static int loggedFS_unlink(const char *path)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = unlink(path);
    loggedfs_log(aPath,"unlink",res,"unlink %s",aPath);
    if(res == -1)
        return -errno;

    return 0;
}

static int loggedFS_rmdir(const char *path)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = rmdir(path);
    loggedfs_log(aPath,"rmdir",res,"rmdir %s",aPath);
    if(res == -1)
        return -errno;

    return 0;
}

static int loggedFS_symlink(const char *from, const char *to)
{
    int res;

    char *aTo = getAbsolutePath(to);
    to = getRelativePath(to);

    res = symlink(from, to);

    loggedfs_log( aTo,"symlink",res,"symlink from %s to %s",aTo,from);
    if(res == -1)
        return -errno;
    else
        lchown(to, fuse_get_context()->uid, fuse_get_context()->gid);

    return 0;
}

static int loggedFS_rename(const char *from, const char *to)
{
    int res;
    char *aFrom=getAbsolutePath(from);
    char *aTo=getAbsolutePath(to);
    from=getRelativePath(from);
    to=getRelativePath(to);
    res = rename(from, to);
    loggedfs_log( aFrom,"rename",res,"rename %s to %s",aFrom,aTo);
    if(res == -1)
        return -errno;

    return 0;
}

static int loggedFS_link(const char *from, const char *to)
{
    int res;

    char *aFrom=getAbsolutePath(from);
    char *aTo = getAbsolutePath(to);
    from=getRelativePath(from);
    to = getRelativePath(to);

    res = link(from, to);
    loggedfs_log( aTo,"link",res,"hard link from %s to %s",aTo,aFrom);
    if(res == -1)
        return -errno;
    else
        lchown(to, fuse_get_context()->uid, fuse_get_context()->gid);

    return 0;
}

static int loggedFS_chmod(const char *path, mode_t mode)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = chmod(path, mode);
    loggedfs_log(aPath,"chmod",res,"chmod %s to %o",aPath, mode);
    if(res == -1)
        return -errno;

    return 0;
}

static char* getusername(uid_t uid)
{
struct passwd *p=getpwuid(uid);
if (p!=NULL)
	return p->pw_name;
return NULL;
}

static char* getgroupname(gid_t gid)
{
struct group *g=getgrgid(gid);
if (g!=NULL)
	return g->gr_name;
return NULL;
}

static int loggedFS_chown(const char *path, uid_t uid, gid_t gid)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = lchown(path, uid, gid);

    char* username = getusername(uid);
    char* groupname = getgroupname(gid);

    if (username!=NULL && groupname!=NULL)
	    loggedfs_log(aPath,"chown",res,"chown %s to %d:%d %s:%s",aPath, uid, gid, username, groupname);
    else
	   loggedfs_log(aPath,"chown",res,"chown %s to %d:%d",aPath, uid, gid);
    if(res == -1)
        return -errno;

    return 0;
}

static int loggedFS_truncate(const char *path, off_t size)
{
    int res;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = truncate(path, size);
    loggedfs_log(aPath,"truncate",res,"truncate %s to %d bytes",aPath, size);
    if(res == -1)
        return -errno;

    return 0;
}

#if (FUSE_USE_VERSION==25)
static int loggedFS_utime(const char *path, struct utimbuf *buf)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = utime(path, buf);
    loggedfs_log(aPath,"utime",res,"utime %s",aPath);
    if(res == -1)
        return -errno;

    return 0;
}

#else


static int loggedFS_utimens(const char *path, const struct timespec ts[2])
{
    int res;
    struct timeval tv[2];

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);

    tv[0].tv_sec = ts[0].tv_sec;
    tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec;
    tv[1].tv_usec = ts[1].tv_nsec / 1000;

    res = utimes(path, tv);

    loggedfs_log(aPath,"utimens",res,"utimens %s",aPath);
    if (res == -1)
        return -errno;

    return 0;
}

#endif

static int loggedFS_open(const char *path, struct fuse_file_info *fi)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = open(path, fi->flags);

	// what type of open ? read, write, or read-write ?
	if (fi->flags & O_RDONLY) {
		 loggedfs_log(aPath,"open-readonly",res,"open readonly %s",aPath);
	}
	else if (fi->flags & O_WRONLY) {
		 loggedfs_log(aPath,"open-writeonly",res,"open writeonly %s",aPath);
	}
	else if (fi->flags & O_RDWR) {
		loggedfs_log(aPath,"open-readwrite",res,"open readwrite %s",aPath);
	}
	else  loggedfs_log(aPath,"open",res,"open %s",aPath);

    if(res == -1)
        return -errno;

    close(res);
    return 0;
}

static int loggedFS_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi)
{
    int fd;
    int res;

    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    (void) fi;

    fd = open(path, O_RDONLY);
    if(fd == -1) {
        res = -errno;
        loggedfs_log(aPath,"read",-1,"read %d bytes from %s at offset %d",size,aPath,offset);
        return res;
    } else {
        loggedfs_log(aPath,"read",0,"read %d bytes from %s at offset %d",size,aPath,offset);
    }

    res = pread(fd, buf, size, offset);

    if(res == -1)
        res = -errno;
    else loggedfs_log(aPath,"read",0, "%d bytes read from %s at offset %d",res,aPath,offset);

    close(fd);
    return res;
}

static int loggedFS_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    int fd;
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    (void) fi;

    fd = open(path, O_WRONLY);
    if(fd == -1) {
        res = -errno;
        loggedfs_log(aPath,"write",-1,"write %d bytes to %s at offset %d",size,aPath,offset);
        return res;
    } else {
        loggedfs_log(aPath,"write",0,"write %d bytes to %s at offset %d",size,aPath,offset);
    }

    res = pwrite(fd, buf, size, offset);

    if(res == -1)
        res = -errno;
    else loggedfs_log(aPath,"write",0, "%d bytes written to %s at offset %d",res,aPath,offset);

    close(fd);
    return res;
}

static int loggedFS_statfs(const char *path, struct statvfs *stbuf)
{
    int res;
    char *aPath=getAbsolutePath(path);
    path=getRelativePath(path);
    res = statvfs(path, stbuf);
    loggedfs_log(aPath,"statfs",res,"statfs %s",aPath);
    if(res == -1)
        return -errno;

    return 0;
}

static int loggedFS_release(const char *path, struct fuse_file_info *fi)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    (void) path;
    (void) fi;
    return 0;
}

static int loggedFS_fsync(const char *path, int isdatasync,
                          struct fuse_file_info *fi)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    (void) path;
    (void) isdatasync;
    (void) fi;
    return 0;
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int loggedFS_setxattr(const char *path, const char *name, const char *value,
                             size_t size, int flags)
{
    int res = lsetxattr(path, name, value, size, flags);
    if(res == -1)
        return -errno;
    return 0;
}

static int loggedFS_getxattr(const char *path, const char *name, char *value,
                             size_t size)
{
    int res = lgetxattr(path, name, value, size);
    if(res == -1)
        return -errno;
    return res;
}

static int loggedFS_listxattr(const char *path, char *list, size_t size)
{
    int res = llistxattr(path, list, size);
    if(res == -1)
        return -errno;
    return res;
}

static int loggedFS_removexattr(const char *path, const char *name)
{
    int res = lremovexattr(path, name);
    if(res == -1)
        return -errno;
    return 0;
}
#endif /* HAVE_SETXATTR */

static void usage(char *name)
{
     fprintf(stderr, "Usage:\n");
     fprintf(stderr, "%s [-h] | [-l log-file] [-c config-file] [-f] [-p] [-e] /directory-mountpoint\n",name);
     fprintf(stderr, "Type 'man loggedfs' for more details\n");
     return;
}

static
bool processArgs(int argc, char *argv[], LoggedFS_Args *out)
{
    // set defaults
    out->isDaemon = true;

    out->fuseArgc = 0;
    out->configFilename=NULL;

    // pass executable name through
    out->fuseArgv[0] = argv[0];
    ++out->fuseArgc;

    // leave a space for mount point, as FUSE expects the mount point before
    // any flags
    out->fuseArgv[1] = NULL;
    ++out->fuseArgc;
    opterr = 0;

    int res;

    bool got_p = false;

// We need the "nonempty" option to mount the directory in recent FUSE's
// because it's non empty and contains the files that will be logged.
//
// We need "use_ino" so the files will use their original inode numbers,
// instead of all getting 0xFFFFFFFF . For example, this is required for
// logging the ~/.kde/share/config directory, in which hard links for lock
// files are verified by their inode equivalency.

#ifdef __APPLE__
  #define COMMON_OPTS "use_ino"
#else
  #define COMMON_OPTS "nonempty,use_ino"
#endif

    while ((res = getopt (argc, argv, "hpfec:l:")) != -1)
    {
        switch (res)
        {
        case 'h':
            usage(argv[0]);
            return false;
        case 'f':
            out->isDaemon = false;
            // this option was added in fuse 2.x
            PUSHARG("-f");
            printf("LoggedFS not running as a daemon\n");
            break;
        case 'p':
            PUSHARG("-o");
            PUSHARG("allow_other,default_permissions," COMMON_OPTS);
            got_p = true;
            printf("LoggedFS running as a public filesystem\n");
            break;
        case 'e':
#ifndef __APPLE__
            PUSHARG("-o");
            PUSHARG("nonempty");
            printf("Using existing directory\n");
#else
            dprintf(2, "WARNING -e: osxfuse can't understand nonempty option\n");
#endif
            break;
        case 'c':
            out->configFilename=optarg;
            printf("Configuration file : %s\n",optarg);
            break;

        default:

            break;
        }
    }

    if (!got_p)
    {
        PUSHARG("-o");
        PUSHARG(COMMON_OPTS);
    }
#undef COMMON_OPTS

    if(optind+1 <= argc)
    {
        out->mountPoint = argv[optind++];
        out->fuseArgv[1] = out->mountPoint;
    }
    else
    {
        fprintf(stderr,"Missing mountpoint\n");
        usage(argv[0]);
        return false;
    }

    // If there are still extra unparsed arguments, pass them onto FUSE..
    if(optind < argc)
    {
        assert(out->fuseArgc < MaxFuseArgs);

        while(optind < argc)
        {
            assert(out->fuseArgc < MaxFuseArgs);
            out->fuseArgv[out->fuseArgc++] = argv[optind];
            ++optind;
        }
    }

    if(!isAbsolutePath( out->mountPoint ))
    {
        fprintf(stderr,"You must use absolute paths "
                "(beginning with '/') for %s\n",out->mountPoint);
        return false;
    }
    return true;
}

int main(int argc, char *argv[])
{
    char* input=new char[2048]; // 2ko MAX input for configuration
    sqlite3 *db;
    pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_t t;



    umask(0);
    fuse_operations loggedFS_oper;
    // in case this code is compiled against a newer FUSE library and new
    // members have been added to fuse_operations, make sure they get set to
    // 0..
    memset(&loggedFS_oper, 0, sizeof(fuse_operations));
    loggedFS_oper.init		= loggedFS_init;
    loggedFS_oper.getattr	= loggedFS_getattr;
    loggedFS_oper.access	= loggedFS_access;
    loggedFS_oper.readlink	= loggedFS_readlink;
    loggedFS_oper.readdir	= loggedFS_readdir;
    loggedFS_oper.mknod	= loggedFS_mknod;
    loggedFS_oper.mkdir	= loggedFS_mkdir;
    loggedFS_oper.symlink	= loggedFS_symlink;
    loggedFS_oper.unlink	= loggedFS_unlink;
    loggedFS_oper.rmdir	= loggedFS_rmdir;
    loggedFS_oper.rename	= loggedFS_rename;
    loggedFS_oper.link	= loggedFS_link;
    loggedFS_oper.chmod	= loggedFS_chmod;
    loggedFS_oper.chown	= loggedFS_chown;
    loggedFS_oper.truncate	= loggedFS_truncate;
#if (FUSE_USE_VERSION==25)
    loggedFS_oper.utime       = loggedFS_utime;
#else
    loggedFS_oper.utimens	= loggedFS_utimens;
#endif
    loggedFS_oper.open	= loggedFS_open;
    loggedFS_oper.read	= loggedFS_read;
    loggedFS_oper.write	= loggedFS_write;
    loggedFS_oper.statfs	= loggedFS_statfs;
    loggedFS_oper.release	= loggedFS_release;
    loggedFS_oper.fsync	= loggedFS_fsync;
#ifdef HAVE_SETXATTR
    loggedFS_oper.setxattr	= loggedFS_setxattr;
    loggedFS_oper.getxattr	= loggedFS_getxattr;
    loggedFS_oper.listxattr	= loggedFS_listxattr;
    loggedFS_oper.removexattr= loggedFS_removexattr;
#endif


    for(int i=0; i<MaxFuseArgs; ++i)
        loggedfsArgs->fuseArgv[i] = NULL; // libfuse expects null args..

    if (processArgs(argc, argv, loggedfsArgs))
    {
        printf("LoggedFS starting at %s.\n",loggedfsArgs->mountPoint);

        if (loggedfsArgs->configFilename!=NULL)
        {

            if (strcmp(loggedfsArgs->configFilename,"-")==0)
            {
                printf("Using stdin configuration\n");
                memset(input,0,2048);
                char *ptr=input;

                int size=0;
                do {
                    size=fread(ptr,1,1,stdin);
                    ptr++;
                }while (!feof(stdin) && size>0);
                config.loadFromXmlBuffer(input);
            }
            else
            {
                printf("Using configuration file %s.\n",loggedfsArgs->configFilename);
                config.loadFromXmlFile(loggedfsArgs->configFilename);
            }
        }

        printf("chdir to %s\n",loggedfsArgs->mountPoint);
        chdir(loggedfsArgs->mountPoint);
        savefd = open(".", 0);

        int err;
        if ((err = sqlite3_open("/tmp/fslogs.db", &db)) != SQLITE_OK) {
            printf("Error opening sqlite database : %s\n", sqlite3_errstr(err));
            return 0;
        }

        if (sqlite3_exec(db, "SELECT 1 from logs", NULL, NULL, NULL) != SQLITE_OK)
        {
            printf("Creating table logs...\n");
            char create_logs[] = "CREATE TABLE logs ("\
                                 "id       INTEGER PRIMARY KEY AUTOINCREMENT,"\
                                 "str      TEXT,"\
                                 "uploaded INT"\
                                 ");";
            int rc = sqlite3_exec(db, create_logs, NULL, NULL, NULL);
            if (rc != SQLITE_OK)
                printf("ERROR creating logs: %s\n", sqlite3_errmsg(db));
            else
                printf("Success creating table logs\n");
        }
        env.db = db;
        env.cond_var = &cond_var;
        env.m = &m;

        if (pthread_create(&t, NULL, &upload_thread, NULL) < 0)
        {
            dprintf(2, "ERROR pthread_create failed\n");
            // TODO: ERROR
        }

#if (FUSE_USE_VERSION==25)
        fuse_main(loggedfsArgs->fuseArgc,
                  const_cast<char**>(loggedfsArgs->fuseArgv), &loggedFS_oper);
#else
        fuse_main(loggedfsArgs->fuseArgc,
                  const_cast<char**>(loggedfsArgs->fuseArgv), &loggedFS_oper, NULL);
#endif
        printf("LoggedFS closing.\n");

    }
}
