#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>

#include "arch/io/disk.hpp"
#include "clustering/administration/main/directory_lock.hpp"

bool check_existence(const base_path_t& base_path) {
    return 0 == access(base_path.path().c_str(), F_OK);
}

bool check_dir_emptiness(const base_path_t& base_path) {
    DIR *dp;
    struct dirent *ep;

    set_errno(0);
    dp = opendir(base_path.path().c_str());
    if (dp == NULL) {
        return false;
    }
    guarantee_err(get_errno() == 0, "Error while reading directory");
    while ((ep = readdir(dp)) != NULL) {
        if (strcmp(ep->d_name, ".") != 0 && strcmp(ep->d_name, "..") != 0) {
            closedir(dp);
            return false;
        }
    }
    closedir(dp);
    return true;
}

directory_lock_t::directory_lock_t(const base_path_t &path, bool create, bool *created_out) :
    directory_path(path),
    created(false),
    initialize_done(false) {

    *created_out = false;
    bool dir_exists = check_existence(directory_path);

    if (!dir_exists) {
        if (!create) {
            throw directory_missing_exc_t(directory_path);
        }
        int mkdir_res;
        do {
            mkdir_res = mkdir(directory_path.path().c_str(), 0755);
        } while (mkdir_res == -1 && get_errno() == EINTR);
        if (mkdir_res != 0) {
            throw directory_create_failed_exc_t(get_errno(), directory_path);
        }
        created = true;
        *created_out = true;

        // Call fsync() on the parent directory to guarantee that the newly
        // created directory's directory entry is persisted to disk.
        warn_fsync_parent_directory(directory_path.path().c_str());
    } else if (create && check_dir_emptiness(directory_path)) {
        created = true;
        *created_out = true;
    }

    directory_fd.reset(::open(directory_path.path().c_str(), O_RDONLY));
    if (directory_fd.get() == INVALID_FD) {
        throw directory_open_failed_exc_t(get_errno(), directory_path);
    }

    if (flock(directory_fd.get(), LOCK_EX | LOCK_NB) != 0) {
        throw directory_locked_exc_t(directory_path);
    }
}

directory_lock_t::~directory_lock_t() {
    // Only delete the directory if we created it and haven't finished initialization
    if (created && !initialize_done) {
        remove_directory_recursive(directory_path.path().c_str());
    }
}

void directory_lock_t::directory_initialized() {
    guarantee(directory_fd.get() != INVALID_FD);
    initialize_done = true;
}

void directory_lock_t::change_ownership(gid_t group_id, const std::string &group_name,
                                        uid_t user_id, const std::string &user_name) {
    if (group_id != INVALID_GID || user_id != INVALID_UID) {
        if (fchown(directory_fd.get(), user_id, group_id) != 0) {
            throw std::runtime_error(strprintf("Failed to change ownership of data "
                                               "directory '%s' to '%s:%s': %s",
                                               directory_path.path().c_str(),
                                               user_name.c_str(),
                                               group_name.c_str(),
                                               errno_string(get_errno()).c_str()));
        }
    }
}
