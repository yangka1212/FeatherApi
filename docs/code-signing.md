# Code signing policy

## Current status

Code signing is not active yet. FeatherApi is preparing an application to the
SignPath Foundation. Existing releases, including v2.0.0, are unsigned. The
presence of this policy, the MIT license, or EXE version information does not
mean a release has a trusted digital signature.

If the application is approved, the planned signing provider attribution is:

> Free code signing provided by [SignPath.io](https://signpath.io), certificate by
> [SignPath Foundation](https://signpath.org).

The certificate publisher would be SignPath Foundation. File metadata may name
the FeatherApi maintainer separately. This page will be updated when production
signing is activated.

## Team and responsibilities

- Author and maintainer: [yangka1212](https://github.com/yangka1212).
- Reviewer: [yangka1212](https://github.com/yangka1212), responsible for reviewing
  external contributions before they are included in a signed release.
- Signing approver: [yangka1212](https://github.com/yangka1212), responsible for
  manually approving each production signing request in SignPath.

Signing team members must enable multi-factor authentication on GitHub and
SignPath before production signing is enabled. Approval includes reviewing the
release source, build workflow, artifact version, and requested signing policy.

## Build and release policy

Only FeatherApi binaries built from this public repository on GitHub-hosted
Windows runners are eligible for signing. Ordinary CI artifacts are explicitly
named unsigned. Pull requests cannot request production signatures.

The manual signing workflow must be dispatched on an existing version tag. It
builds the immutable commit recorded by that workflow run and verifies that the
checkout matches the run's source commit before building, so SignPath's origin
metadata identifies the source used for the binary. The release version comes
from the triggering tag. The workflow runs the offline tests, checks EXE metadata
against that tag, and submits the build artifact to SignPath. It requires
configured SignPath credentials and a signing policy allowing that tag ref with
manual approval. The returned EXE must have a valid Authenticode signature and a
timestamp before it can be packaged or added to a draft GitHub Release.

The release ZIP contains FeatherApi.exe and the MIT license. SHA-256 checksums
are computed after signing and packaging. Published release files are never
silently replaced by this workflow. The maintainer reviews and publishes the
draft release, with a link to this policy in its notes.

## Privacy policy

FeatherApi does not collect analytics, telemetry, or crash reports. It does not
require an account or automatically synchronize workspaces to a cloud service.
Requests and workspace data are saved locally at
`%LOCALAPPDATA%\FeatherApi-Win32\data.json`.

Network connections are made when the user sends an HTTP request or imports an
OpenAPI/Swagger document from a URL. The selected endpoint receives the request
data supplied by the user, such as headers, authentication values, request
bodies, or selected file uploads. HTTP redirects and system proxy settings can
affect the destination or route. The operator of the selected service determines
how it processes the received data.

This program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it.

The separate CI and signing services receive source/build artifacts and
maintainer account information; they do not receive users' local workspaces from
the application. Their service policies are available at
[GitHub](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
and [SignPath](https://signpath.io/privacy-policy).
