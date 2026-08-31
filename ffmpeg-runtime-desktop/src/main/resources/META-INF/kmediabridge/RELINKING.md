# Replacing the bundled runtime

Pass a directory containing a compatible `manifest.properties` and the native
libraries to `BundledFfmpegNativeDriver.load(replacementDirectory = ...)`.
The loader verifies that directory's own hashes and runtime-reported license,
version, configure arguments, and ABI before it opens media.

The complete rebuild recipe is published at:
https://github.com/SuvioMedia/KMediaBridgeNative/tree/main/native
