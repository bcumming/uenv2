function setup() {
    set -u
    export CLUSTER_NAME=arapiles

    bats_load_library bats-support
    bats_load_library bats-assert
    load ./common

    export PATH="$UENV_BIN_PATH:$PATH"
    unset UENV_MOUNT_LIST

    export TMP=$DATA/scratch
    rm -rf $TMP
    mkdir -p $TMP

    # these tests need more than one task on a node, launched by Slurm, and a
    # squashfs-mount that understands --join (i.e. a fuse build)
    if ! command -v srun >/dev/null 2>&1; then
        skip "srun is not available"
    fi
    if [[ -z "${SLURM_JOB_ID:-}" ]]; then
        skip "not running inside a Slurm allocation"
    fi
    if ! squashfs-mount --help 2>&1 | grep -q -- '--join'; then
        skip "squashfs-mount does not support --join (not a fuse build)"
    fi

    export IMG=$SQFS_LIB/apptool/standalone/tool.squashfs
    export SRUN="srun --oversubscribe -N1"
}

function teardown() {
    # the barrier deliberately abandons its IPC objects when a rendezvous
    # fails, so tests that fail one must clean up after themselves
    srun --oversubscribe -N1 -n1 bash -c \
        'rm -f /dev/shm/uenv-run* /dev/shm/sem.uenv-run*' >/dev/null 2>&1 || true
}

@test "--join mounts once and every task shares it" {
    run $SRUN -n4 uenv run --join $IMG -- \
        bash -c 'mp=${UENV_MOUNT_LIST##*:}; echo "SEEN $(stat -c %d $mp) $(ls -1 $mp | wc -l)"'
    assert_success
    # four tasks, all reporting the same device id and a non-empty mount
    [ "$(echo "$output" | grep -c '^SEEN ')" -eq 4 ]
    [ "$(echo "$output" | grep '^SEEN ' | awk '{print $2}' | sort -u | wc -l)" -eq 1 ]
    refute_output --partial "SEEN 0"
}

@test "--join leaves every task in the working directory it started in" {
    # regression: setns() into the leader's mount namespace drops the cwd, so
    # followers used to land on "/" and every relative path -- ./a.out, the
    # usual way a job names its binary -- failed for all but the leader.
    cd $TMP
    touch marker
    run $SRUN -n4 uenv run --join $IMG -- \
        bash -c 'echo "PWD $(pwd) $(test -e marker && echo found || echo missing)"'
    assert_success
    [ "$(echo "$output" | grep -c '^PWD ')" -eq 4 ]
    [ "$(echo "$output" | grep '^PWD ' | awk '{print $2, $3}' | sort -u | wc -l)" -eq 1 ]
    assert_output --partial "found"
    refute_output --partial "missing"
}

@test "--join rejects a shared memory segment that is too small" {
    # regression: open_existing() used to mmap sizeof(T) bytes of whatever was
    # under the name and every peer died with SIGBUS on first access. The tag
    # is derived from SLURM_JOBID/SLURM_STEPID, so it is plantable.
    local tag=jointest$$
    $SRUN -n1 bash -c \
        "f=/dev/shm/uenv-run_shm-squashfs-mount-\$SLURM_JOBID-$tag; : > \$f; chmod 600 \$f"

    run $SRUN -n2 bash -c \
        "export SLURM_STEPID=$tag; exec uenv run --join $IMG -- true"
    assert_failure
    assert_output --partial "too small to hold"
    refute_output --partial "Bus error"
}

@test "--join without the Slurm environment is refused" {
    run env -u SLURM_STEPID -u SLURM_STEP_ID -u SLURM_JOBID -u SLURM_JOB_ID \
        -u SLURM_STEP_TASKS_PER_NODE -u SLURM_NODEID \
        uenv run --join $IMG -- true
    assert_failure
    assert_output --partial "--join requires SLURM"
}

@test "--join inside a uenv session is refused" {
    run uenv run $IMG -- uenv run --join $IMG -- true
    assert_failure
    assert_output --partial "already running"
}

@test "--join keeps the mount alive when the leader exits first" {
    # The leader is an arbitrary rank. If the squashfuse daemons were tied to
    # its command instead of to the supervisor, the leader returning first
    # would leave every other rank with "Transport endpoint is not connected".
    cat >$TMP/leader-exit.sh <<'EOF'
#!/bin/bash
mnt=${UENV_MOUNT_LIST##*:}
# the leader is the rank whose command still owns the forked squashfuse daemon
role=follower
if ps -eo ppid=,comm= | awk -v me=$$ '$2=="squashfs-mount" && $1==me{f=1}END{exit !f}'; then
    role=leader
fi
if [[ $role == leader ]]; then
    echo "LEADER rank=${SLURM_PROCID} exiting immediately"
    exit 0
fi
# outlive the leader, then keep using the mount
sleep 5
if out=$(ls -1 "$mnt" 2>&1) && [[ -n $out ]]; then
    echo "FOLLOWER rank=${SLURM_PROCID} ok nfiles=$(echo "$out" | wc -l)"
else
    echo "FOLLOWER rank=${SLURM_PROCID} lost [${out//$'\n'/ }]"
fi
EOF
    chmod +x $TMP/leader-exit.sh

    run $SRUN -n4 uenv run --join $IMG -- $TMP/leader-exit.sh
    assert_success
    [ "$(echo "$output" | grep -c '^LEADER ')" -eq 1 ]
    [ "$(echo "$output" | grep -c '^FOLLOWER .* ok ')" -eq 3 ]
    refute_output --partial "Transport endpoint"
}

@test "--join releases the mount once the step ends" {
    # The supervisor outlives every rank's command; what ends it is
    # slurmstepd's SIGKILL sweep of the step's cgroup. If that stopped
    # holding, daemons and the namespaces pinning the images would accumulate
    # on the node, one set per step.
    run $SRUN -n4 uenv run --join $IMG -- true
    assert_success

    run $SRUN -n1 bash -c \
        'pgrep -x squashfs-mount || echo NO_DAEMONS; ls /dev/shm/uenv-run* 2>/dev/null || echo NO_IPC'
    assert_success
    assert_output --partial "NO_DAEMONS"
    assert_output --partial "NO_IPC"
}
