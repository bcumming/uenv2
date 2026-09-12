#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <err.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <fmt/format.h>
extern "C" {
#include <squashfuse/ll.h>
}
#include <spdlog/spdlog.h>

#include <uenv/mount.h>
#include <uenv/mount_rootless.h>
#include <util/cgroup.h>
#include <util/expected.h>
#include <util/proc_barrier.h>
#include <util/ready_fork.h>
#include <util/setns.h>
#include <util/shell.h>

namespace {
util::expected<int, std::string>
openat_wrap(int dirfd, std::filesystem::path file, int oflag) {
    int fd = ::openat(dirfd, file.c_str(), oflag);
    if (fd < 0) {
        return util::unexpected(fmt::format("opening {} failed with {}",
                                            file.string(), strerror(errno)));
    }
    return fd;
}

util::expected<void, std::string> write_wrap(int fd, const void* buf,
                                             size_t count) {
    ssize_t n = ::write(fd, buf, count);
    if (n < 0) {
        return util::unexpected(fmt::format("write({}, {}, {}) failed with {}",
                                            fd, (char*)buf, count,
                                            strerror(errno)));
    }
    if (static_cast<size_t>(n) != count) {
        return util::unexpected(
            fmt::format("short write: wrote {} of {} bytes", n, count));
    }
    return {};
}

} // namespace

namespace uenv {

namespace rootless {

bool daemon_process_will_cleanup() {
    // The supervisor is cleaned up by slurmstepd sweeping the step's cgroup
    // (see supervisor_main), so it is only safe to fork where Slurm
    // tracks processes with cgroups. Under proctrack/linuxproc or
    // proctrack/pgid a supervisor reparented away from its task may never
    // be reaped, and a leaked daemon plus the namespace pinning its image
    // is worse than the mount ending with this rank's command: mount
    // in-process there instead, loudly.
    const auto cgroup = util::current_cgroup();
    if (!cgroup || !util::cgroup_is_slurm_managed(cgroup.value())) {
        return false;
    }
    return true;
}

util::expected<void, std::string> clone_new_ns_and_set_dumpable() {
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        return util::unexpected(
            fmt::format("unshare failed: {}", strerror(errno)));
    }
    // enable coredumps, otherwise we cannot write to uid_map/gid_map etc.
    if (prctl(PR_SET_DUMPABLE, 1) != 0) {
        return util::unexpected(
            fmt::format("prctl(PR_SET_DUMPABLE) failed: {}", strerror(errno)));
    }

    return {};
}

// Same effect as `unshare --mount --map-root-user`
util::expected<void, std::string> unshare_mount_map_root() {
    spdlog::trace("become fake root");
    int uid = getuid(); // get current uid
    int gid = getgid();

    if (auto r = clone_new_ns_and_set_dumpable(); !r) {
        return r;
    }

    if (auto r = mount(std::nullopt, "/", std::nullopt, MS_REC | MS_PRIVATE,
                       nullptr);
        !r) {
        return r;
    }

    // map current uid to root
    char buf[256];
    auto proc_uid_map = openat_wrap(AT_FDCWD, "/proc/self/uid_map", O_WRONLY);
    if (!proc_uid_map) {
        return util::unexpected(proc_uid_map.error());
    }
    snprintf(buf, sizeof(buf), "0 %d 1", uid);
    if (auto r = write_wrap(proc_uid_map.value(), buf, strlen(buf)); !r) {
        return r;
    }
    if (close(proc_uid_map.value()) != 0) {
        return util::unexpected(
            fmt::format("close failed: {}", strerror(errno)));
    }

    // write /proc/self/gid_setgroups -> deny
    auto proc_setgroups =
        openat_wrap(AT_FDCWD, "/proc/self/setgroups", O_WRONLY);
    if (!proc_setgroups) {
        return util::unexpected(proc_setgroups.error());
    }
    if (auto r = write_wrap(proc_setgroups.value(), "deny", 4); !r) {
        return r;
    }
    if (close(proc_setgroups.value()) != 0) {
        return util::unexpected(
            fmt::format("close failed: {}", strerror(errno)));
    }

    // map gid  to root group
    auto proc_gid_map = openat_wrap(AT_FDCWD, "/proc/self/gid_map", O_WRONLY);
    if (!proc_gid_map) {
        return util::unexpected(proc_gid_map.error());
    }
    snprintf(buf, sizeof(buf), "0 %d 1", gid);
    if (auto r = write_wrap(proc_gid_map.value(), buf, strlen(buf)); !r) {
        return r;
    }
    if (close(proc_gid_map.value()) != 0) {
        return util::unexpected(
            fmt::format("close failed: {}", strerror(errno)));
    }

    return {};
}

// go back to effective user
util::expected<void, std::string> map_effective_user(uid_t uid, gid_t gid) {
    if (auto r = clone_new_ns_and_set_dumpable(); !r) {
        return r;
    }

    // map current user id to root
    char buf[256];
    spdlog::trace("map_effective_user({}, {})", uid, gid);
    auto proc_uid_map = openat_wrap(AT_FDCWD, "/proc/self/uid_map", O_WRONLY);
    if (!proc_uid_map) {
        return util::unexpected(proc_uid_map.error());
    }
    sprintf(buf, "%d 0 1", uid);
    if (auto r = write_wrap(proc_uid_map.value(), buf, strlen(buf)); !r) {
        return r;
    }
    if (close(proc_uid_map.value()) != 0) {
        return util::unexpected(
            fmt::format("close failed: {}", strerror(errno)));
    }

    // note: setgroups is already "deny" here, inherited from the enclosing
    // fake-root namespace (unshare_mount_map_root) -- once a namespace's
    // setgroups is "deny", all descendant namespaces are permanently locked
    // to "deny" too, so writing "allow" here would always fail with EPERM.

    auto proc_gid_map = openat_wrap(AT_FDCWD, "/proc/self/gid_map", O_WRONLY);
    if (!proc_gid_map) {
        return util::unexpected(proc_gid_map.error());
    }
    sprintf(buf, "%d 0 1", gid);
    if (auto r = write_wrap(proc_gid_map.value(), buf, strlen(buf)); !r) {
        return r;
    }
    if (close(proc_gid_map.value()) != 0) {
        return util::unexpected(
            fmt::format("close failed: {}", strerror(errno)));
    }

    return {};
}

// restore `dumps_allowed` (the caller's dumpable state before it was forced
// on -- see mount_and_join_ns) and disallow gaining any new privileges. Not
// safe to call until every peer that needs /proc/<this pid>/ns/* has already
// opened it: once dumpable goes to 0, that access is refused.
util::expected<void, std::string> lock_down(const bool dumps_allowed) {
    // set explicitly rather than only clearing: map_effective_user's own
    // unshare(CLONE_NEWUSER) already reset DUMPABLE to 0 once, so it cannot
    // be assumed to still be 1 here even when dumps_allowed is true.
    if (prctl(PR_SET_DUMPABLE, dumps_allowed ? 1 : 0) != 0) {
        return util::unexpected(
            fmt::format("prctl(PR_SET_DUMPABLE) failed: {}", strerror(errno)));
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return util::unexpected("PR_SET_NO_NEW_PRIVS failed");
    }
    return {};
}

// The forked child shares the parent's entire call stack. A plain `return`
// from an error path here would unwind back into the caller's normal control
// flow *inside the child*, which would then go on to do whatever the parent
// is supposed to do next (e.g. mount further images, exec the target
// command) -- running it twice, once in each process. Error paths in the
// child must therefore terminate the child directly instead of returning.
[[noreturn]] static void child_fail(const std::string& msg) {
    spdlog::error("{}", msg);
    _exit(1);
}

fuse_lowlevel_ops make_fuse_lowlevel_options() {
    fuse_lowlevel_ops options;
    memset(&options, 0, sizeof(fuse_lowlevel_ops));

    options.getattr = sqfs_ll_op_getattr;
    options.opendir = sqfs_ll_op_opendir;
    options.releasedir = sqfs_ll_op_releasedir;
    options.readdir = sqfs_ll_op_readdir;
    options.lookup = sqfs_ll_op_lookup;
    options.open = sqfs_ll_op_open;
    options.create = sqfs_ll_op_create;
    options.release = sqfs_ll_op_release;
    options.read = sqfs_ll_op_read;
    options.readlink = sqfs_ll_op_readlink;
    options.listxattr = sqfs_ll_op_listxattr;
    options.getxattr = sqfs_ll_op_getxattr;
    options.forget = sqfs_ll_op_forget;
    options.statfs = stfs_ll_op_statfs;

    return options;
}

// Mount a squashfs image in the background (forked process), unmount the image
// when the parent process exits (shell is closed).
// Adapted from `squashfuse_ll` written by Dave Vasilevsky <dave@vasilevsky.ca>
// Ref: https://github.com/vasi/squashfuse/blob/master/ll_main.c
//
// Returns the pid of the forked daemon, which is PDEATHSIG-tied to the
// caller: the caller owns the mount's lifetime.
util::expected<pid_t, std::string> do_sqfs_ll_mount(const mount_pair& entry,
                                                    bool fuse_single_threaded) {
    spdlog::trace("do_sqfs_ll_mount");

    // use a pipe to synchronize parent and child process
    auto rf = util::ready_fork::create();
    if (!rf) {
        return util::unexpected(rf.error());
    }

    pid_t pid = rf->fork();
    if (pid < 0) {
        return util::unexpected(
            fmt::format("fork() failed: {}", strerror(errno)));
    }

    if (pid == 0) {
        // kill the fuse process when the parent exits. prctl(2) documents a
        // race here: the parent can die in the gap between fork() returning
        // and this call actually arming the signal, in which case we are
        // already silently reparented and nothing will ever signal us. The
        // kernel reparents atomically, so comparing getppid() to the pid
        // rf captured (in the parent, before fork()) tells us definitively
        // whether that already happened.
        if (prctl(PR_SET_PDEATHSIG, SIGHUP) != 0) {
            child_fail(fmt::format("prctl(PR_SET_PDEATHSIG) failed: {}",
                                   strerror(errno)));
        }
        if (getppid() != rf->parent_pid()) {
            child_fail("parent process exited before PDEATHSIG could be "
                       "armed");
        }

        // do not listen for SIGINT (ctrl+c)
        signal(SIGINT, SIG_IGN);

        // setup dummy fuse_args to make sqfs_ll_open happy
        std::string dummy{"dummy"};
        std::vector<char*> dummy_args{const_cast<char*>(dummy.c_str()),
                                      nullptr};
        fuse_args args{.argc = 1, .argv = dummy_args.data(), .allocated = 0};

        // define squashfs file system operations
        auto sqfs_ll_ops = make_fuse_lowlevel_options();

        // fuse_daemonize() will unconditionally clobber fds 0-2.
        // If we get one of these file descriptors in sqfs_ll_open,
        // we're going to have a bad time. Just make sure that all
        // these fds are open before opening the image file, that way
        // we must get a different fd.
        while (true) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd == -1) {
                // Can't open /dev/null, how bizarre! However,
                // fuse_deamonize won't clobber fds if it can't
                // open /dev/null either, so we ought to be OK.
                break;
            }
            if (fd > 2) {
                // fds 0-2 are now guaranteed to be open.
                close(fd);
                break;
            }
        }

        // the child process starts fuse and informs the calling process via
        // pipe when done

        if (auto ll = sqfs_ll_open(entry.sqfs.c_str(), 0); ll == nullptr) {
            child_fail("sqfs_ll_open_failed");
        } else {
            // startup fuse
            sqfs_ll_chan ch;
            sqfs_err sqfs_ret =
                sqfs_ll_mount(&ch, entry.mount.c_str(), &args, &sqfs_ll_ops,
                              sizeof(sqfs_ll_ops), ll);
            if (sqfs_ret == SQFS_OK) {
                if (sqfs_ll_daemonize(true /*foreground*/) != -1) {
                    // setup signal handlers and enter fuse_session_loop
                    if (fuse_set_signal_handlers(ch.session) != -1) {
                        // inform parent process that sqfs has been mounted with
                        // signal handlers in place
                        if (auto ok = rf->notify_ready(); !ok) {
                            child_fail(ok.error());
                        }

                        int err;
                        if (!fuse_single_threaded) {
                            fuse_loop_config config{.clone_fd = 1,
                                                    .max_idle_threads = 10};
                            err = fuse_session_loop_mt(ch.session, &config);
                        } else {
                            err = fuse_session_loop(ch.session);
                        }
                        // nonzero means the loop faulted (e.g. a
                        // pthread_create failure under a node's
                        // thread/process limits, an I/O error on
                        // /dev/fuse) rather than the ordinary
                        // PDEATHSIG-turned-SIGHUP/unmount shutdown, which
                        // returns 0. Nothing waits on this child's exit
                        // status, so this log -- not the exit code -- is
                        // what's actually observable.
                        if (err != 0) {
                            spdlog::warn(
                                "fuse session loop exited with an error: {}",
                                err);
                        }

                        fuse_remove_signal_handlers(ch.session);
                    } else {
                        child_fail("set signal handlers failed.");
                    }

                } else {
                    child_fail("daemonize failed");
                }
                sqfs_ll_destroy(ll);
                free(ll);

                // Rely on OS cleanup for the actual unmount, to avoid
                // additional synchronization when mounts are nested:
                // `sqfs_ll_unmount` would have to be executed in reverse
                // order. When this line is reached, the computational tasks
                // have already finished.
                // `sqfs_ll_unmount` doesn't fail, but it calls
                // `fuse_session_unmount`, which prints an error message
                // `fuse: failed to unmount ...: No such file or directory`
                //
                // sqfs_ll_unmount(&ch, entry.mount.c_str());
                //
                // Still free the `fuse_session` allocated by
                // `sqfs_ll_mount`/`fuse_session_new`, otherwise it leaks.
                fuse_session_destroy(ch.session);
            } else {
                switch (sqfs_ret) {
                case SQFS_ERR:
                    child_fail(fmt::format("SQFS_ERR {} ", strerror(errno)));
                case SQFS_BADFORMAT:
                    child_fail("SQFS_BADFORMAT (unsupported file format)");
                case SQFS_BADVERSION:
                    child_fail("SQFS_BADVERSION");
                case SQFS_BADCOMP:
                    child_fail("SQFS_BADCOMP");
                case SQFS_UNSUP:
                    child_fail("SQFS_UNSUP, unsupported feature");
                default:
                    child_fail("SQFS unknown failure");
                }
            }
        }
        fuse_opt_free_args(&args);
        _exit(0);
    }

    // parent block on pipe until fusemount has finished.
    if (auto ok = rf->wait_ready(); !ok) {
        return util::unexpected(
            fmt::format("mounting squashfs image {} at {} failed: {}",
                        entry.sqfs.string(), entry.mount.string(), ok.error()));
    }

    return pid;
}

// Point fds 0-2 at /dev/null. The supervisor outlives the task that forked
// it, and holding that task's stdout/stderr pipes open would stall
// slurmstepd's I/O forwarding at the end of the step. The fuse daemons need
// no equivalent: fuse_daemonize already clobbers 0-2.
util::expected<void, std::string> detach_stdio() {
    int fd = ::open("/dev/null", O_RDWR);
    if (fd < 0) {
        return util::unexpected(
            fmt::format("opening /dev/null failed: {}", strerror(errno)));
    }
    for (int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(fd, target) < 0) {
            return util::unexpected(fmt::format(
                "dup2(/dev/null, {}) failed: {}", target, strerror(errno)));
        }
    }
    if (fd > STDERR_FILENO) {
        ::close(fd);
    }
    return {};
}

// Fork one squashfuse daemon per image, report readiness to the task that
// forked this process, then sleep until killed.
[[noreturn]] void supervisor_main(util::ready_fork& rf,
                                  const uenv::mount_list& mounts,
                                  bool fuse_single_threaded) {
    // Deliberately no PR_SET_PDEATHSIG: this process must outlive the task
    // that forked it, which goes on to exec the user's command.
    //
    // Slurm ends it instead. --join only exists inside a job step, every
    // process a step creates is in the step's cgroup, and slurmstepd SIGKILLs
    // whatever is left in that cgroup once the last task exits. The mount's
    // lifetime is therefore the step's lifetime on this node. So never
    // setsid() or otherwise leave that cgroup.
    //
    // Ignore the signals Slurm sends the step first: on a time limit or
    // scancel it signals the whole cgroup, then gives the ranks KillWait
    // seconds to shut down. Dying on that SIGTERM would unmount under ranks
    // that are still flushing or checkpointing.
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    std::vector<pid_t> daemons;
    auto abandon = [&daemons](const std::string& msg) {
        // the daemons hold a copy of rf's write end: leaving them running
        // would keep the task blocked in wait_ready() instead of seeing
        // this failure.
        for (auto pid : daemons) {
            ::kill(pid, SIGKILL);
        }
        child_fail(msg);
    };

    for (auto& entry : mounts) {
        auto pid = do_sqfs_ll_mount(entry, fuse_single_threaded);
        if (!pid) {
            abandon(pid.error());
        }
        daemons.push_back(*pid);
    }

    spdlog::info("mount supervisor {} owns {} mount(s) in cgroup {}", getpid(),
                 daemons.size(), util::current_cgroup().value_or("<unknown>"));

    // only now: every mount is up and any error already reported. Not fatal
    // if it fails.
    if (auto r = detach_stdio(); !r) {
        spdlog::warn("mount supervisor could not detach stdio: {}", r.error());
    }

    if (auto ok = rf.notify_ready(); !ok) {
        abandon(ok.error());
    }

    while (true) {
        pause();
    }
}

// Fork the supervisor that owns the mounts and block until it reports every
// image mounted. Must be called after unshare_mount_map_root(), so that the
// supervisor and its daemons live in the namespaces the mounts belong to.
util::expected<void, std::string>
fork_mount_supervisor(const uenv::mount_list& mounts,
                      bool fuse_single_threaded) {
    auto rf = util::ready_fork::create();
    if (!rf) {
        return util::unexpected(rf.error());
    }

    pid_t pid = rf->fork();
    if (pid < 0) {
        return util::unexpected(
            fmt::format("fork() failed: {}", strerror(errno)));
    }
    if (pid == 0) {
        supervisor_main(*rf, mounts, fuse_single_threaded);
    }

    if (auto ok = rf->wait_ready(); !ok) {
        return util::unexpected(
            fmt::format("mount supervisor failed: {}", ok.error()));
    }

    return {};
}

util::expected<void, std::string>
unshare_and_mount(const uenv::mount_list& mounts, bool fuse_single_threaded) {
    if (auto r = unshare_mount_map_root(); !r) {
        return r;
    }
    for (auto& entry : mounts) {
        if (auto r = do_sqfs_ll_mount(entry, fuse_single_threaded); !r) {
            return util::unexpected(r.error());
        }
    }
    return {};
}

util::expected<void, std::string>
mount_and_join_ns(const std::string& tag, int ntasks,
                  const uenv::mount_list& mounts, bool fuse_single_threaded,
                  uid_t uid, gid_t gid) {
    // capture the caller's dumpable state before unshare_and_mount forces it
    // on, so that lock_down can restore it once the mounts are ready.
    // Any non-zero result (SUID_DUMP_USER or SUID_DUMP_ROOT) counts as
    // "dumpable"; only SUID_DUMP_DISABLE (0) means lock_down must turn it
    // back off.
    const int dumpable = prctl(PR_GET_DUMPABLE);
    if (dumpable < 0) {
        return util::unexpected(
            fmt::format("prctl(PR_GET_DUMPABLE) failed: {}", strerror(errno)));
    }
    const bool dumps_allowed = dumpable != 0;

    if (ntasks == 1) {
        // no peers to join, just mount and drop privileges
        if (auto r = unshare_and_mount(mounts, fuse_single_threaded); !r) {
            return r;
        }
        if (auto r = map_effective_user(uid, gid); !r) {
            return r;
        }
        return lock_down(dumps_allowed);
    }

    auto barrier = util::proc_barrier::create(tag, ntasks);
    if (!barrier) {
        return util::unexpected(barrier.error());
    }

    if (barrier->is_leader()) {
        if (auto r = unshare_mount_map_root(); !r) {
            return r;
        }

        // A supervisor process owns the mounts, not this process: this
        // process execs the user's command, and the leader is an arbitrary
        // rank, so tying the daemons here would take the mount away from
        // every other rank on the node when that one rank's command returns.
        if (!daemon_process_will_cleanup()) {
            spdlog::warn(
                "squashfuse daemon will not get cleaned up automatically.");
            for (auto& entry : mounts) {
                if (auto r = do_sqfs_ll_mount(entry, fuse_single_threaded);
                    !r) {
                    return util::unexpected(r.error());
                }
            }
        } else if (auto r = fork_mount_supervisor(mounts, fuse_single_threaded);
                   !r) {
            return r;
        }

        // exit fake-root. Note: this does its own unshare(CLONE_NEWUSER |
        // CLONE_NEWNS), so it moves us into a *new* mount namespace (a copy
        // of the current one) -- the other tasks must join this final
        // namespace, not the one from before this call, so ready() below
        // must come after it.
        if (auto r = map_effective_user(uid, gid); !r) {
            return r;
        }

        // publish our pid and release the tasks blocked in the barrier so
        // they can join our (now final) namespaces *before* we lock down
        // below.
        if (auto r = barrier->ready(); !r) {
            return r;
        }

        // wait until every other local task has finished joining our
        // namespaces -- only then is it safe to change our dumpable state.
        if (auto r = barrier->wait_peers(); !r) {
            return r;
        }

    } else {
        // setns() into a mount namespace drops the working directory: the
        // cwd this process holds belongs to the namespace it is leaving, and
        // the kernel leaves it sitting on the new namespace's root instead.
        // Every relative path the command was given -- `./a.out`, the usual
        // way a job names its binary -- would then resolve against "/" in
        // the followers while resolving normally in the leader. Capture it
        // before the join so it can be restored after.
        const auto cwd = util::cwd();

        // the leader publishes the PID that entered the squashfs namespace,
        // so joining its namespaces gives us the same view
        auto r = util::namespaces_join(barrier->leader_pid(), {"user", "mnt"},
                                       barrier->leader_start_time());

        // signal the leader regardless of outcome, so it isn't left waiting
        // out the full barrier timeout for a peer that will never succeed.
        if (auto sr = barrier->signal_done(); !sr) {
            spdlog::warn("barrier signal_done failed: {}", sr.error());
        }
        if (!r) {
            return r;
        }

        // restore the working directory inside the joined namespace. Not
        // fatal if it fails: the command may not care about its cwd, and
        // failing the whole task would be a worse outcome than the warning.
        if (!cwd) {
            spdlog::warn("unable to determine the working directory to "
                         "restore after joining the mount namespace");
        } else {
            try {
                std::filesystem::current_path(*cwd);
            } catch (...) {
                spdlog::warn("unable to restore the working directory {}",
                             cwd->string());
            }
        }
    }

    // drop capabilities for every process
    if (auto r = lock_down(dumps_allowed); !r) {
        return r;
    }

    // treated as fatal: barrier teardown (sem_unlink/shm_unlink) failing
    // means the IPC objects backing this barrier were not cleaned up. If this
    // turns out to be noisy in practice (e.g. spurious races on teardown),
    // it may be safe to downgrade to a warning.
    if (auto r = barrier->end(); !r) {
        return util::unexpected(
            fmt::format("barrier teardown failed: {}", r.error()));
    }

    return {};
}
} // namespace rootless
} // namespace uenv
