function setup() {
    # set the cluster name to be arapiles
    # this is required for tests to work when run on a vCluster
    # that sets this variable
    set -u
    # USER may be unset in privileged containers (e.g. GitHub Actions with --privileged)
    export USER="${USER:-$(whoami)}"
    export CLUSTER_NAME=arapiles

    #echo "BATS_LIB_PATH $BATS_LIB_PATH" 1>&3
    bats_load_library bats-support
    bats_load_library bats-assert
    load ./common

    export PATH="$UENV_BIN_PATH:$PATH"

    unset UENV_MOUNT_LIST

    # set up location for creation of working repos
    export TMP=$DATA/scratch
    rm -rf $TMP
    mkdir -p $TMP

    # force the system name to arapiles via a user config file.
    # a system config (e.g. /etc/uenv/config.toml on Alps) sets system_name and
    # takes priority over the CLUSTER_NAME environment variable, so tests point
    # XDG_CONFIG_HOME at a throwaway user config that wins over the system one.
    export XDG_CONFIG_HOME=$TMP/user-config
    mkdir -p $XDG_CONFIG_HOME/uenv
    cat > $XDG_CONFIG_HOME/uenv/config.toml <<EOF
system_name = 'arapiles'
EOF

    # remove the bash function uenv, if an older version of uenv is installed on
    # the system
    unset -f uenv
}

function teardown() {
    :
}

@test "noargs" {
    run uenv
    assert_output --partial "uenv version $(cat $SRC_PATH/VERSION)"
    assert_success
}

@test "--version" {
    run uenv --version
    assert_output "$(cat $SRC_PATH/VERSION)"
    assert_success
}

@test "image ls" {
    run uenv --repo=$REPOS/apptool image ls
    assert_success
    assert_line --index 0 --partial "repo apptool"
    assert_line --index 1 --regexp "^uenv\s+arch\s+system\s+id"
    assert_output --regexp "app/42.0:v1\s+zen3\s+arapiles"
    assert_output --regexp "app/43.0:v1\s+zen3\s+arapiles"
    assert_output --regexp "tool/17.3.2:v1\s+zen3\s+arapiles"

    # use space instead of = for the --repo option, to check
    # that both methods work.
    run uenv --repo $REPOS/apptool image ls --no-header
    assert_success
    refute_line --regexp "^uenv\s+arch\s+system\s+id"
    assert_line --regexp "app/42.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "app/43.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "tool/17.3.2:v1\s+zen3\s+arapiles"

    # test a label of the form "@system" with no name
    run uenv --repo=$REPOS/apptool image ls --no-header @$CLUSTER_NAME
    assert_success
    refute_line --regexp "^uenv\s+arch\s+system\s+id"
    assert_line --regexp "app/42.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "app/43.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "tool/17.3.2:v1\s+zen3\s+arapiles"

    # test a label of the form "@'*'" with no name
    # this matches all clusters (overriding the default)
    run uenv --repo=$REPOS/apptool image ls --no-header @'*'
    assert_success
    refute_line --regexp "^uenv\s+arch\s+system\s+id"
    assert_line --regexp "app/42.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "app/43.0:v1\s+zen3\s+arapiles"
    assert_line --regexp "tool/17.3.2:v1\s+zen3\s+arapiles"

    run uenv --repo=$REPOS/apptool image ls --no-header app
    assert_success
    assert_line --partial "app/42.0:v1"
    assert_line --partial "app/43.0:v1"
    refute_line --partial "tool/17.3.2:v1"

    run uenv --repo=$REPOS/apptool image ls --no-header app/43.0
    assert_success
    refute_line --partial "app/42.0:v1"
    assert_line --partial "app/43.0:v1"
    refute_line --partial "tool/17.3.2:v1"

    run uenv --repo=$REPOS/apptool image ls --no-header tool
    assert_success
    refute_line --partial "app/42.0:v1"
    refute_line --partial "app/43.0:v1"
    assert_line --partial "tool/17.3.2:v1"

    run uenv --repo=$REPOS/apptool image ls wombat
    assert_success
    assert_output --partial "repo apptool"
    assert_output --partial "no matching uenv"

    # empty output if --no-header is used and there are no matches
    run uenv --repo=$REPOS/apptool image ls wombat --no-header
    assert_success
    assert_output ""

    run uenv --repo=/wombat image ls --no-header
    assert_failure
    assert_output --partial "the repository '/wombat' does not exist"

    run uenv --repo=$REPOS/apptool image ls --no-header
    assert_success
    assert_line --partial "app/42.0:v1"
    assert_line --partial "app/43.0:v1"
    assert_line --partial "tool/17.3.2:v1"

    # test --json output
    run uenv --repo=$REPOS/apptool image ls --json app
    assert_success
    jq_output="$(echo "$output" | jq '.records | length')"
    assert_equal "$jq_output" "2"
    # each record carries a repo field identifying its source
    jq_output="$(echo "$output" | jq '.records[0].repo.name')"
    assert_equal "$jq_output" '"apptool"'

    # empty results is not an error
    run uenv --repo=$REPOS/apptool image ls --json doesnotexist
    assert_success
    jq_output="$(echo "$output" | jq '.records | length')"
    assert_equal "$jq_output" "0"
}

@test "status" {
    export RP=$REPOS/apptool

    run uenv --repo=$RP status
    assert_success
    assert_line --index 0 "there is no uenv loaded"

    run uenv --repo=$RP status --format=short
    assert_success
    run uenv --repo=$RP status --format=views
    assert_success

    run uenv --repo=$RP status --error-if-unset
    assert_failure
}

@test "repo status" {
    export RP=$REPOS/apptool

    #
    # check the different methods for providing the repo location
    #

    # using --repo flag to uenv: name is derived from the path basename
    run uenv --repo=$RP repo status
    assert_success
    assert_line --index 0 "apptool:$RP is readwrite"

    # as a positional argument to the repo status command itself
    run uenv repo status $RP
    assert_success
    assert_line --index 0 "cli:$RP is readwrite"

    # no error for a path that does not exist
    run uenv repo status /wombat
    assert_success
    assert_line --index 0 "cli:/wombat is not a repository"

    # TODO:
    # - check a read-only repo
    # - check an invalid repo
}

@test "repo create" {
    # using UENV_REPO_PATH env variable
    RP=$(mktemp -d $TMP/create-XXXXXX)
    run uenv repo create $RP
    assert_success
    assert [ -d  $RP ]
    assert [ -e  $RP/index.db ]
    run sqlite3 $RP/index.db .dump
    assert_line --partial "CREATE TABLE images"
    assert_line --partial "CREATE TABLE uenv"
    assert_line --partial "CREATE TABLE tags"
    assert_line --partial "CREATE VIEW records AS"

    # create a repo in the same location
    # this should be an error
    run uenv repo create $RP
    assert_failure
    assert_line --partial "unable to create repository"

    RP=$TMP/ro
    mkdir --mode=-w $RP
    # run with logging to check for detailed "Permission denied" message
    run uenv -v repo create $RP/test
    assert_failure
    assert_line --partial "error: unable to create repository"
    assert_line --partial "Permission denied"
}

@test "run" {
    export CLUSTER_NAME=arapiles

    #
    # check that run looks up images in the repo and mounts at the correct location
    #
    run uenv -v --repo=$REPOS/apptool run tool -- findmnt /user-tools
    assert_success
    assert_output --regexp "meta"

    run uenv --repo=$REPOS/apptool run app/42.0:v1 -- ls /user-environment
    assert_success
    assert_output --regexp "meta"

    #
    # check --view and @system
    #
    run uenv --repo=$REPOS/apptool run --view=tool tool@$CLUSTER_NAME -- tool
    assert_success
    assert_output "hello tool"

    run uenv --repo=$REPOS/apptool run --view=app app/42.0:v1 -- app
    assert_success
    assert_output "hello app"

    # the tool view in the tool environment:
    # - sets TOOLVAR=SET
    # - unsets TOOLCONFLICT
    # check that this is the case
    export TOOLCONFLICT="yes"
    unset TOOLVAR
    run uenv --repo=$REPOS/apptool run --view=tool tool -- printenv
    assert_success
    assert_line "TOOLVAR=SET"
    refute_line "TOOLCONFLICT=yes"
    unset TOOLCONFLICT

    # the tool uenv provides a default-view called defaultwombat
    # - sets WOMBAT=soup
    # check that this is the case
    unset WOMBAT
    run uenv --repo=$REPOS/apptool run tool -- bash -c 'echo WOMBAT=$WOMBAT'
    assert_success
    assert_line "WOMBAT=soup"

    # verify that disabling default views works
    export WOMBAT=sleepy
    run uenv --repo=$REPOS/apptool run --no-default-view tool -- bash -c 'echo WOMBAT=$WOMBAT'
    assert_success
    assert_line "WOMBAT=sleepy"

    #
    # check --view works when reading meta data from inside a standalone sqfs file
    #

    run uenv --repo=$REPOS/apptool run --view=tool $SQFS_LIB/apptool/standalone/tool.squashfs -- tool
    assert_success
    assert_output "hello tool"

    #
    # check --view works when the view has a different name to the uenv
    #

    run uenv --repo=$REPOS/apptool run --view=wombat tool -- printenv WOMBAT
    assert_success
    assert_output "soup for $USER"

    #
    # check that relative paths can be used for squashfs images
    #

    relativepath=./$(realpath -s --relative-to=$PWD $SQFS_LIB/apptool/standalone/tool.squashfs)
    run uenv --repo=$REPOS/apptool run --view=tool $relativepath -- tool
    assert_success
    assert_output "hello tool"
}

@test "start" {
    UENV_REPO_PATH=$REPOS/apptool
    export CLUSTER_NAME=arapiles

    # set UENV_BATS_SKIP_START to skip the start tests.
    # used when running tests in a no-tty env (e.g. in GitHub action runner).
    [[ -n "${UENV_BATS_SKIP_START-}" ]] && skip

    #
    # check that run looks up images in the repo and mounts at the correct location
    #
    run uenv --repo=$REPOS/apptool start tool <<EOF
echo 'hello world'
exit
EOF
    assert_failure
    assert_output --partial "must be run in an interactive shell"

    run uenv --repo=$REPOS/apptool start --ignore-tty tool <<EOF
echo 'hello world'
exit
EOF
    assert_success

    #
    # check that attempting to call 'uenv run' in an already running session produces
    # a user-friendly error message.
    #
    run uenv --repo=$REPOS/apptool start --ignore-tty tool <<EOF
uenv run tool -- echo "hi"
exit
EOF
    assert_failure
    assert_output --partial "a uenv session is already running"

    run uenv --repo=$REPOS/apptool start --ignore-tty tool <<EOF
uenv start --ignore-tty tool
exit
EOF
    assert_failure
    assert_output --partial "a uenv session is already running"
}

@test "multi repo --repo flag" {
    RP1=$(mktemp -d $TMP/repo1-XXXXXX)
    RP2=$(mktemp -d $TMP/repo2-XXXXXX)
    run uenv repo create $RP1
    assert_success
    run uenv repo create $RP2
    assert_success

    # two paths: names are derived from the path basenames
    N1=$(basename $RP1)
    N2=$(basename $RP2)
    run uenv --repo=$RP1,$RP2 repo status
    assert_success
    assert_line "$N1:$RP1 is readwrite"
    assert_line "$N2:$RP2 is readwrite"
    [ "${#lines[@]}" -eq 2 ]

    # reversing the order reverses the names too
    run uenv --repo=$RP2,$RP1 repo status
    assert_success
    assert_line "$N2:$RP2 is readwrite"
    assert_line "$N1:$RP1 is readwrite"
    [ "${#lines[@]}" -eq 2 ]

    # name=path syntax assigns explicit names
    run uenv --repo=first=$RP1,second=$RP2 repo status
    assert_success
    assert_line "first:$RP1 is readwrite"
    assert_line "second:$RP2 is readwrite"
    [ "${#lines[@]}" -eq 2 ]

    # a non-existent path in the list is an error
    run uenv --repo=$RP1,/wombat repo status
    assert_failure

    # populate repos with distinct images and verify image ls returns both
    run uenv --repo=first=$RP1 image add app/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/app42.squashfs
    assert_success
    run uenv --repo=second=$RP2 image add tool/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs
    assert_success

    run uenv --repo=first=$RP1,second=$RP2 image ls --no-header
    assert_success
    assert_line --partial "app/1.0:v1"
    assert_line --partial "tool/1.0:v1"

    # table output has a section header per repo
    run uenv --repo=first=$RP1,second=$RP2 image ls
    assert_success
    assert_output --partial "repo first"
    assert_output --partial "repo second"

    # json output: each record carries a repo field
    run uenv --repo=first=$RP1,second=$RP2 image ls --json
    assert_success
    jq_output="$(echo "$output" | jq '[.records[].repo.name] | sort | unique | join(",")')"
    assert_equal "$jq_output" '"first,second"'
}

@test "multi repo toml config" {
    RP1=$(mktemp -d $TMP/repo1-XXXXXX)
    RP2=$(mktemp -d $TMP/repo2-XXXXXX)
    run uenv repo create $RP1
    assert_success
    run uenv repo create $RP2
    assert_success

    # write a config with two [[repositories]] entries — this was previously
    # broken because parse_repository_array rejected arrays with more than one
    # entry.
    XDG=$TMP/xdg
    mkdir -p $XDG/uenv
    cat > $XDG/uenv/config.toml <<EOF
system_name = 'arapiles'

[[repositories]]
name = 'first'
path = '$RP1'

[[repositories]]
name = 'second'
path = '$RP2'
EOF

    # use XDG_CONFIG_HOME to redirect user config, and a fake HOME to suppress
    # default repo detection.
    FAKE_HOME=$(mktemp -d $TMP/home-XXXXXX)
    run env HOME=$FAKE_HOME XDG_CONFIG_HOME=$XDG uenv repo status
    assert_success
    assert_line "first:$RP1 is readwrite"
    assert_line "second:$RP2 is readwrite"

    # populate repos with distinct images and verify image ls searches both
    run env HOME=$FAKE_HOME XDG_CONFIG_HOME=$XDG uenv --repo=first=$RP1 image add \
        app/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/app42.squashfs
    assert_success
    run env HOME=$FAKE_HOME XDG_CONFIG_HOME=$XDG uenv --repo=second=$RP2 image add \
        tool/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs
    assert_success

    run env HOME=$FAKE_HOME XDG_CONFIG_HOME=$XDG uenv image ls --no-header
    assert_success
    assert_line --partial "app/1.0:v1"
    assert_line --partial "tool/1.0:v1"
}

@test "multi repo run" {
    RP1=$(mktemp -d $TMP/repo1-XXXXXX)
    RP2=$(mktemp -d $TMP/repo2-XXXXXX)
    run uenv repo create $RP1
    assert_success
    run uenv repo create $RP2
    assert_success

    # put app42 in repo1 and app43 in repo2, both registered as app/1.0:v1
    run uenv --repo=first=$RP1 image add app/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/app42.squashfs
    assert_success
    run uenv --repo=second=$RP2 image add app/1.0:v1@$CLUSTER_NAME%zen3 $SQFS_LIB/apptool/standalone/app43.squashfs
    assert_success

    # repo1 has higher priority: app --version should print 42.0
    run uenv --repo=first=$RP1,second=$RP2 run --view=app app -- app --version
    assert_success
    assert_output "42.0"

    # with reversed priority repo2 wins: app --version should print 43.0
    run uenv --repo=second=$RP2,first=$RP1 run --view=app app -- app --version
    assert_success
    assert_output "43.0"

    # with only repo2 the match comes from there: app --version should print 43.0
    run uenv --repo=second=$RP2 run --view=app app -- app --version
    assert_success
    assert_output "43.0"
}

@test "image add" {
    # using UENV_REPO_PATH env variable
    RP=$(mktemp -d $TMP/create-XXXXXX)
    run uenv repo create $RP
    assert_success

    run uenv --repo=$RP image add wombat/24:v1@arapiles%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs
    assert_success
    assert_output --partial "the uenv wombat/24:v1@arapiles%zen3"
    assert_output --partial "was added to $RP"
    refute_output --partial "warning"
    refute_output --partial "error"

    # trying to add the same image should be a failure
    run uenv --repo=$RP image add wombat/24:v1@arapiles%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs
    assert_failure
    assert_output --partial "uenv already exists"

    # trying to add the same image with a different label should be an error
    run uenv --repo=$RP image add numbat/24:v1@arapiles%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs
    assert_failure
    assert_output --partial "already in the repo"

    # trying to add the same image via label should work
    run uenv --repo=$RP image add numbat/24:v1@arapiles%zen3 wombat/24:v1@arapiles%zen3
    assert_success
    assert_output --partial "the uenv numbat/24:v1@arapiles%zen3"
    assert_output --partial "was added to $RP"
    refute_output --partial "warning"
    refute_output --partial "error"

    run uenv --repo=$RP image ls --no-header
    assert_success
    assert_output --partial "numbat/24:v1"
    assert_output --partial "wombat/24:v1"

    # the manifest that identifies the image is persisted alongside it, and
    # the <hash> used to store/index the image is exactly the sha256 of that
    # manifest.
    wombat_sha=$(uenv --repo=$RP image inspect --format='{sha256}' wombat/24:v1)
    wombat_manifest=$RP/images/$wombat_sha/manifest.json
    [ -f "$wombat_manifest" ]
    [ "$wombat_sha" = "$(sha256sum "$wombat_manifest" | cut -d' ' -f1)" ]

    # the manifest hash is deterministic: adding identical squashfs content
    # under a different label reuses the same store/images/<hash> directory
    # rather than storing a second copy.
    numbat_sha=$(uenv --repo=$RP image inspect --format='{sha256}' numbat/24:v1)
    [ "$wombat_sha" = "$numbat_sha" ]

    run uenv --repo=$RP image ls --no-header
    assert_success
    assert_line --partial 'wombat/24:v1'
    assert_line --partial 'numbat/24:v1'
    # assert that the number of lines in the output is 2: one for each uenv
    [ "${#lines[@]}" -eq "2" ]

    # add a different image with a different label
    run uenv --repo=$RP image add bilby/24:v1@arapiles%zen3 $SQFS_LIB/apptool/standalone/app42.squashfs
    assert_success
    refute_output --partial "warning"
    refute_output --partial "error"

    run uenv --repo=$RP image ls --no-header
    assert_success
    assert_line --partial 'wombat/24:v1'
    assert_line --partial 'numbat/24:v1'
    assert_line --partial 'bilby/24:v1'
    [ "${#lines[@]}" -eq "3" ]

    # test that moving an image into place works
    # we have to move the image from the same file system as the repo
    # so we use a copy in TMP/scratch
    sqfs_file=$TMP/app43.squashfs
    cp $SQFS_LIB/apptool/standalone/app43.squashfs $sqfs_file
    [ -f  $sqfs_file ]
    run uenv -vv --repo=$RP image add --move quokka/24:v1@arapiles%zen3 $sqfs_file
    assert_success
    refute_output --partial "warning"
    refute_output --partial "error"
    # assert that the input file was moved properly
    ! [ -f  $sqfs_file ]

    run uenv --repo=$RP image ls --no-header
    assert_success
    assert_line --partial 'wombat/24:v1'
    assert_line --partial 'numbat/24:v1'
    assert_line --partial 'bilby/24:v1'
    assert_line --partial 'quokka/24:v1'
    [ "${#lines[@]}" -eq "4" ]

    # adding a new label that aliases an image already in the repo (by label)
    # is the sanctioned way to retag, and succeeds cleanly
    run uenv --repo=$RP image add numbat/24:replica@arapiles%zen3 numbat/24:v1
    assert_success
    refute_output --partial "warning"
    refute_output --partial "error"

    run uenv --repo=$RP image ls --no-header
    assert_success
    assert_line --partial 'numbat/24:v1'
    assert_line --partial 'numbat/24:replica'
    [ "${#lines[@]}" -eq "5" ]

    # TODO:
    # - check a read-only repo
}

@test "image inspect" {
    export UENV_REPO_PATH=$REPOS/apptool
    export CLUSTER_NAME=arapiles

    run uenv --repo=$REPOS/apptool image inspect tool
    assert_success

    run uenv --repo=$REPOS/apptool image inspect --format='{name}:{tag}' tool
    assert_success
    assert_output "tool:v1"

    # check a format string that contains no fields
    run uenv --repo=$REPOS/apptool image inspect --format='hello world' tool
    assert_success
    assert_output "hello world"

    # check that the --format argument works with whitespace instead of =
    run uenv --repo=$REPOS/apptool image inspect --format 'hello world' tool
    assert_success
    assert_output "hello world"

    run uenv --repo=$REPOS/apptool image inspect --format='{sha256}' tool
    assert_success
    run uenv --repo=$REPOS/apptool image inspect --format='{sqfs}' tool
    assert_success

    # get the sha and path of the tool squashfs image
    sha=$(uenv --repo=$REPOS/apptool image inspect --format='{sha256}' tool)
    sqfs=$(uenv --repo=$REPOS/apptool image inspect --format='{sqfs}' tool)

    # check that the squashfs file exists
    [ -f $sqfs ]

    # compute the actual sha and validate that it matches the one
    # returned by inspect
    computed_sha=$(sha256sum ${sqfs} | awk '{print $1}')
    [ "$computed_sha" == "$sha" ]
}

@test "image rm" {
    export UENV_REPO_PATH=$(mktemp -d $TMP/create-XXXXXX)
    run uenv repo create $UENV_REPO_PATH
    assert_success

    # add uenv to a repo for us to try removing
    # the wombat uenv have the same sha: the second label is added by aliasing
    # the first, because adding the same squashfs a second time is an error
    uenv --repo=$UENV_REPO_PATH image add wombat/24:rc1@arapiles%zen3 $SQFS_LIB/apptool/standalone/tool.squashfs > /dev/null
    uenv --repo=$UENV_REPO_PATH image add wombat/24:v1@arapiles%zen3  wombat/24:rc1@arapiles%zen3 > /dev/null
    # the bilby images have unique sha
    uenv --repo=$UENV_REPO_PATH image add bilby/24:v1@arapiles%zen3   $SQFS_LIB/apptool/standalone/app42.squashfs > /dev/null
    uenv --repo=$UENV_REPO_PATH image add bilby/24:v2@arapiles%zen3   $SQFS_LIB/apptool/standalone/app43.squashfs > /dev/null

    # test that removing an uenv that is not in the repo is handled correctly
    for pattern in dingo/24:rc1@arapiles%zen3  aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa aaaaaaaaaaaaaaaa
    do
        run uenv --repo=$UENV_REPO_PATH image rm $pattern
        assert_failure
        assert_output "error: no uenv matches $pattern"
    done

    # test that removing a sha with more than one label removes both labels
    sha=$(uenv --repo=$UENV_REPO_PATH image inspect wombat/24:rc1 --format={sha256})

    # assert that the files exist
    [ -d $UENV_REPO_PATH/images/$sha ]

    run uenv --repo=$UENV_REPO_PATH image rm $sha
    assert_success
    assert_output --partial "the following uenv were removed"
    assert_output --partial "wombat/24:rc1"
    assert_output --partial "wombat/24:v1"

    # verify that the sha and wombat image are no longer in the repo
    run uenv --repo=$UENV_REPO_PATH image ls --no-header $sha
    assert_output ''
    run uenv --repo=$UENV_REPO_PATH image ls --no-header wombat
    assert_output ''

    # verify that the file was removed
    [ ! -d $UENV_REPO_PATH/images/$sha ]

    # remove using a label that is the only label attached to a sha
    # - should remove the label
    # - should remove the storage
    pattern=bilby/24:v1
    sha=$(uenv --repo=$UENV_REPO_PATH image inspect $pattern --format={sha256})
    [ -d $UENV_REPO_PATH/images/$sha ]
    run uenv --repo=$UENV_REPO_PATH -vv image rm $pattern
    assert_success
    # verify that the file was removed
    [ ! -d $UENV_REPO_PATH/images/$sha ]
    # check that the uenv was removed from database
    run uenv --repo=$UENV_REPO_PATH image ls --no-header $pattern
    assert_output ""

    # removing a one of multiple labels on the same sha removes the label but leaves the others untouched
    # step 1: add more labels with the same hash as the remaining image
    # (bilby/24:v2) by aliasing it, since re-adding the squashfs is an error
    run uenv --repo=$UENV_REPO_PATH image add wallaby/24:v2@arapiles%zen3   bilby/24:v2@arapiles%zen3 > /dev/null
    run uenv --repo=$UENV_REPO_PATH image add wallaby/24:v3@arapiles%zen3   bilby/24:v2@arapiles%zen3 > /dev/null

    pattern=wallaby/24:v2
    sha=$(uenv --repo=$UENV_REPO_PATH image inspect $pattern --format={sha256})
    [ -d $UENV_REPO_PATH/images/$sha ]

    run uenv --repo=$UENV_REPO_PATH -vv image rm $pattern
    assert_success
    [ -d $UENV_REPO_PATH/images/$sha ]

    pattern=wallaby/24:v3
    run uenv --repo=$UENV_REPO_PATH image rm $pattern
    assert_success
    [ -d $UENV_REPO_PATH/images/$sha ]

    pattern=bilby/24:v2
    run uenv --repo=$UENV_REPO_PATH image rm $pattern
    assert_success
    [ ! -d $UENV_REPO_PATH/images/$sha ]
}

@test "registry credentials without a username" {
    # the username that a token is paired with comes from --username, else
    # $USER: with neither, uenv has to say so instead of guessing.
    #
    # append to the config written by setup(), which sets system_name. the
    # listing service is pointed at a closed local port so that the cases which
    # get *past* credential resolution fail immediately, without a network call.
    cat >> $XDG_CONFIG_HOME/uenv/config.toml <<EOF
[registry]
url = 'https://registry.example.com/uenv'
default_namespace = 'deploy'
listing_url = 'http://127.0.0.1:1/list'
EOF

    printf 'tok\n' > $TMP/tok
    chmod 600 $TMP/tok
    local dst=deploy::app/1.0:v1@arapiles%zen3

    # credentials are resolved before the registry is contacted and before the
    # source squashfs is read, so neither has to exist for this test.
    run env -u USER -u LOGNAME uenv image push --token=$TMP/tok x.squashfs $dst
    assert_failure
    assert_output --partial "--username"

    # with --username the same command gets as far as the listing service
    run env -u USER -u LOGNAME uenv image push --token=$TMP/tok --username=bob \
        x.squashfs $dst
    assert_failure
    refute_output --partial "--username"
    assert_output --partial "unable to get a listing"

    # ... and $USER is used when the flag is absent (note that env requires its
    # options before any NAME=VALUE assignment)
    run env -u LOGNAME USER=bob uenv image push --token=$TMP/tok x.squashfs $dst
    assert_failure
    refute_output --partial "--username"
    assert_output --partial "unable to get a listing"
}
