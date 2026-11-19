ogles2 upload run
=================

The test of a user's report (2026-11-19): texture uploads into 16-bit
formats (RGB565, RGB5_A1, RGBA4) took about 3.7 times longer a texel than
into RGBA8. upload_test makes a 1024x1024x4 2D array texture of each
format and uploads 64 tiles of 256x256 into it twice (a warm-up and the
measured pass), timing the calls. It opens a 640x480 screen of its own
for a few seconds. results/1-upload.txt holds the times.
