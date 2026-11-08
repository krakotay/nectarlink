# Development rules

- Do not add implicit fallback paths. Do not silently substitute another GPU
  backend, software processing, a codec, or a transport when the requested path
  fails. Fail the operation explicitly with actionable diagnostics; do not
  swallow the underlying error. Unsupported configurations must be visible.
- The new Linux mirroring renderer targets Vulkan. Do not add OpenGL or CPU
  rendering compatibility paths for old systems.
- Implement changes before running builds. Build the requested APK and Debian
  package at the end; the user installs them and performs device testing.
- Preserve the user's Debian package installation workflow. Do not install local
  desktop binaries, replace launchers, or install APKs unless explicitly asked.
