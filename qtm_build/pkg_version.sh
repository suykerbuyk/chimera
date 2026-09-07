# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
# Derive the package version/revision from the nearest reachable
# <X.Y.Z>-myriad* git tag.  SOURCED, never executed:
#
#     - . /context/qtm_build/pkg_version.sh
#
# as the first command of every packaging task in qb.yaml.  Exports
# PKG_VERSION and PKG_REVISION, used BOTH for the staging directory name
# (make_debian_pkg.sh takes the .deb filename from `basename $PWD`) and as the
# positional arguments to make_debian_pkg.sh / make_rpm_pkg.sh, so the
# filename and the control/spec Version cannot drift apart.
#
# Rules this file exists under:
#   * POSIX sh only.  qb runs task commands under /bin/sh when a task declares
#     no `shell:`, and none of ours do -- that is dash on the Ubuntu builders.
#     No arrays, no ${v//x/y}, no [[ ]].
#   * `set -e` is already in effect and sourcing runs in that same shell, so
#     every command must succeed or be tolerated with `|| :`.  Never end this
#     file, or any branch of it, on a bare `[ ... ] && cmd`: a false test makes
#     the `.` return 1 and kills the build.
#   * Never `exit` -- that tears down the caller's shell.  `return <n>` is
#     enough: `.` is a special builtin, so a non-zero return aborts the task.
#   * dpkg forbids '-' in a revision; rpm forbids it in Version and Release
#     alike.  Nothing produced here may contain one.
#
# Shapes produced:
#   on tag 0.0.2-myriad          -> 0.0.2-myriad
#   3 commits past it            -> 0.0.2-myriad.3.g8aea6716
#   12 past 0.0.1-myriad9        -> 0.0.1-myriad9.12.gdeadbee
#   dirty source tree            -> ...dirty
#   no tag reachable             -> 0.0.0-untagged.g8aea6716 (with a warning)
#
# The '.' separators keep it monotonic in dpkg AND rpm: "myriad" sorts before
# "myriad.3.g...", and numeric sub-segments compare numerically in both
# (myriad.10 > myriad.3).  0.0.0-1, the version every package carried before
# this existed, sorts below every real tag in both.

PKG_VERSION=''
PKG_REVISION=''
_pv_src=''
_pv_tag=''
_pv_extra=''
# Overridable only so the fragment can be exercised outside a build pod.
_pv_repo=${PKG_VERSION_REPO:-/context}

# Short sha for the synthesised forms, from what qb computed on the host.
_pv_sha=${QB_BUILD_GIT_SHA:-unknown}
_pv_sha=${_pv_sha%-dirty}
_pv_sha=$(printf '%s' "${_pv_sha}" | cut -c1-8)

if [ -n "${QB_VERSION:-}" ]; then
    # `qb -V VERSION=1.2.3 [-V REVISION=4]`.  qb exports every manifest
    # variable into the container as QB_<NAME>, which is how -V still reaches
    # us with no `variables:` block in qb.yaml.  An explicit version skips git
    # entirely, so it also works where there is no usable .git at all.
    PKG_VERSION=${QB_VERSION}
    PKG_REVISION=${QB_REVISION:-1}
    _pv_src='-V override'
else
    # Defence for a future runner that drops privileges; today qb untars
    # /context and execs as the same uid, so the dubious-ownership check never
    # fires.  It must be --global, not `git -c`: the command scope only counts
    # as protected configuration from git 2.38 and Ubuntu 22.04 ships 2.34.
    git config --global --add safe.directory "${_pv_repo}" >/dev/null 2>&1 || :

    # --long is load-bearing: it forces the "<tag>-<n>-g<abbrev>" form even at
    # n=0, which is what makes the "strip the last two '-' fields" parse below
    # safe.  Without it an exact match returns the bare tag and the strip eats
    # tag components instead.  No --dirty: it would re-hash every tracked file
    # against a tree qb just untarred with cold mtimes; qb already encoded
    # dirtiness in QB_BUILD_GIT_SHA.
    _pv_desc=$(git -C "${_pv_repo}" describe --tags --long \
                   --match '*-myriad*' 2>/dev/null) || _pv_desc=''

    if [ -n "${_pv_desc}" ]; then
        _pv_gsha=${_pv_desc##*-}
        _pv_rest=${_pv_desc%-*}
        _pv_n=${_pv_rest##*-}
        _pv_tag=${_pv_rest%-*}
        if [ "${_pv_n}" != 0 ]; then
            _pv_extra=".${_pv_n}.${_pv_gsha}"
        fi
        _pv_src="git describe ${_pv_desc}"
    elif [ -n "${QB_BUILD_GIT_TAG:-}" ]; then
        # No usable repository in the container -- usually a git worktree
        # checkout, whose .git is a FILE naming a host path that does not exist
        # in the pod.  Fall back to what qb computed on the HOST, where go-git
        # resolves worktrees.  On GitLab this one is CI_COMMIT_TAG, so a
        # release-tag pipeline is exact even under a shallow clone.
        _pv_tag=${QB_BUILD_GIT_TAG%-dirty}
        _pv_src="QB_BUILD_GIT_TAG ${_pv_tag}"
    elif [ -n "${QB_BUILD_GIT_DESCRIBE_TAG:-}" ]; then
        # Nearest ancestor semver tag, also host-computed.  It carries no
        # commit distance, so synthesise ".0.g<sha>": still above the bare tag,
        # below any real ".<n>.g<sha>".  Deliberately NOT QB_BUILD_GIT_DESCRIBE
        # -- qb builds that with no --match, so it happily returns sync-code-*.
        _pv_tag=${QB_BUILD_GIT_DESCRIBE_TAG%-dirty}
        _pv_extra=".0.g${_pv_sha}"
        _pv_src="QB_BUILD_GIT_DESCRIBE_TAG ${_pv_tag}"
    fi

    # The guard that rejects the bare 0.0.0 tag and any sync-code-* leakage.
    case ${_pv_tag} in
        *-myriad*)
            PKG_VERSION=${_pv_tag%%-*}
            PKG_VERSION=${PKG_VERSION#v}
            _pv_suffix=${_pv_tag#*-}
            # A tag with a second '-' (0.0.3-myriad-rc1) would otherwise yield
            # a revision illegal in both formats.
            _pv_suffix=$(printf '%s' "${_pv_suffix}" | tr -- '-' '.')
            PKG_REVISION=${_pv_suffix}${_pv_extra}
            ;;
        *)
            PKG_VERSION=''
            ;;
    esac

    if ! printf '%s' "${PKG_VERSION}" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "pkg_version.sh: WARNING no <X.Y.Z>-myriad* tag reachable from HEAD;" >&2
        echo "  building an unreleasable 0.0.0-untagged package.  Usual causes: a" >&2
        echo "  shallow clone (GIT_DEPTH: 0 in .gitlab-ci.yml), no .git in the" >&2
        echo "  build context, or a git worktree checkout.  To pin it by hand:" >&2
        echo "    qb -V VERSION=X.Y.Z -V REVISION=<rev> <task>" >&2
        PKG_VERSION=0.0.0
        PKG_REVISION="untagged.g${_pv_sha}"
        _pv_src='no tag'
    fi

    # Host-computed dirtiness; suppressed by `qb -u/--unsafe-version`.
    case ${QB_BUILD_GIT_SHA:-} in
        *-dirty) PKG_REVISION="${PKG_REVISION}.dirty" ;;
    esac

    # A lone `-V REVISION=` re-stamps a rebuild of the same tag.
    if [ -n "${QB_REVISION:-}" ]; then
        PKG_REVISION=${QB_REVISION}
        _pv_src="${_pv_src}, revision from -V"
    fi
fi

# Safety net, mainly for -V: a Debian version must start with a digit, and '-'
# must not appear in a revision (dpkg) or in Release (rpm).
if ! printf '%s' "${PKG_VERSION}" | grep -Eq '^[0-9][A-Za-z0-9.+~]*$'; then
    echo "pkg_version.sh: refusing to build with version '${PKG_VERSION}'" >&2
    return 1
fi
# Intersection of what dpkg allows in a revision and rpm in Release, minus the
# '_' and '^' we never emit.  No '-' can survive this.
if ! printf '%s' "${PKG_REVISION}" | grep -Eq '^[A-Za-z0-9.+~]+$'; then
    echo "pkg_version.sh: refusing to build with revision '${PKG_REVISION}'" >&2
    return 1
fi

export PKG_VERSION PKG_REVISION
echo "pkg_version: ${PKG_VERSION}-${PKG_REVISION} (${_pv_src})"

# End on a command that always succeeds -- the caller's `set -e` sees the
# status of the `.` itself.
unset _pv_repo _pv_src _pv_tag _pv_extra _pv_sha _pv_gsha _pv_rest _pv_n \
      _pv_desc _pv_suffix
