# Copyright 2020-2026. Quantum Corporation. All Rights Reserved.
#
# Derive the package version/revision from the <X.Y.Z>-myriad* tags qb found
# on the host.  SOURCED, never executed:
#
#     - . /context/qtm_build/pkg_version.sh
#
# as the first command of every packaging task in qb.yaml.  Exports
# PKG_VERSION and PKG_REVISION, used BOTH for the staging directory name
# (make_debian_pkg.sh takes the .deb filename from `basename $PWD`) and as the
# positional arguments to make_debian_pkg.sh / make_rpm_pkg.sh, so the
# filename and the control/spec Version cannot drift apart.
#
# Nothing here runs git.  The packaging tasks drop .git from their build
# context (context_exclude in qb.yaml), so everything comes from what qb
# computed on the host and exports into the container (qb's
# internal/git/version.go):
#
#   QB_BUILD_GIT_SHA           full HEAD sha (CI_COMMIT_SHA on GitLab)
#   QB_BUILD_GIT_TAG           highest semver tag ON HEAD, or CI_COMMIT_TAG
#                              verbatim on a GitLab tag pipeline
#   QB_BUILD_GIT_DESCRIBE_TAG  highest semver tag on HEAD, else that of the
#                              nearest tagged ancestor
#
# each with a "-dirty" suffix for a modified tree unless qb ran with
# -u/--unsafe-version.  qb's tag filter is any X.Y.Z prefix, not *-myriad*,
# so the guard below still has to reject the bare 0.0.0 tag.  The host walk
# needs the tags and their history, hence GIT_DEPTH: 0 in .gitlab-ci.yml.
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
#   past it                      -> 0.0.2-myriad.g8aea6716
#   past 0.0.1-myriad9           -> 0.0.1-myriad9.g8aea6716
#   dirty source tree            -> ...dirty
#   no tag reachable             -> 0.0.0-untagged.g8aea6716 (with a warning)
#
# No commit count: qb does not compute one, and the tag plus the sha already
# identify the build.  The '.' separator keeps a build past a tag above the
# tag itself in dpkg AND rpm ("myriad" sorts before "myriad.g..."), and the
# next tag above every build past this one.  Two builds past the SAME tag
# order by their sha, which is arbitrary: install a specific one by its full
# version rather than relying on upgrade order.  0.0.0-1, the version every
# package carried before this existed, sorts below every real tag in both.

PKG_VERSION=''
PKG_REVISION=''
_pv_src=''
_pv_tag=''
_pv_extra=''

# Short sha for the past-a-tag and untagged forms.
_pv_sha=${QB_BUILD_GIT_SHA:-unknown}
_pv_sha=${_pv_sha%-dirty}
_pv_sha=$(printf '%s' "${_pv_sha}" | cut -c1-8)

if [ -n "${QB_VERSION:-}" ]; then
    # `qb -V VERSION=1.2.3 [-V REVISION=4]`.  qb exports every manifest
    # variable into the container as QB_<NAME>, which is how -V still reaches
    # us with no `variables:` block in qb.yaml.  An explicit version skips the
    # tag lookup entirely.
    PKG_VERSION=${QB_VERSION}
    PKG_REVISION=${QB_REVISION:-1}
    _pv_src='-V override'
else
    # A release is a HEAD that carries a -myriad tag itself.  Matched by name
    # rather than by being non-empty: on a GitLab tag pipeline this is
    # CI_COMMIT_TAG, which may just as well be a sync-code-* tag.
    _pv_tag=${QB_BUILD_GIT_TAG:-}
    _pv_tag=${_pv_tag%-dirty}
    case ${_pv_tag} in
        *-myriad*)
            _pv_src="QB_BUILD_GIT_TAG ${_pv_tag}"
            ;;
        *)
            _pv_tag=${QB_BUILD_GIT_DESCRIBE_TAG:-}
            _pv_tag=${_pv_tag%-dirty}
            _pv_extra=".g${_pv_sha}"
            _pv_src="QB_BUILD_GIT_DESCRIBE_TAG ${_pv_tag}"
            ;;
    esac

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
        echo "pkg_version.sh: WARNING qb found no <X.Y.Z>-myriad* tag reachable" >&2
        echo "  from HEAD; building an unreleasable 0.0.0-untagged package.  Usual" >&2
        echo "  causes: a shallow clone on the qb host (GIT_DEPTH: 0 in" >&2
        echo "  .gitlab-ci.yml), a clone without its tags, or a build outside qb." >&2
        echo "  To pin it by hand:" >&2
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
unset _pv_src _pv_tag _pv_extra _pv_sha _pv_suffix
