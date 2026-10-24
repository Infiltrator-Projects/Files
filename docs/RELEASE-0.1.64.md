# Files 0.1.64 — Quick Look preview tranche

## Product goal

GJ-03 and GJ-04: inspect a known or located file without opening an external application, while retaining its authoritative URI and the surrounding folder context. Quick Look is opt-in via Space or the selected regular-file menu; the ordinary file-opening command and existing mutation engines are unaffected.

## Ten delivered user capabilities

1. Open the selected regular file's Quick Look with Space outside editable controls.
2. Open Quick Look via a contextual action and the collapsed Inspection and tools menu.
3. Read supported small UTF-8 text documents within the preview.
4. View supported images using background decoding and a GTK paintable.
5. Inspect other regular-file formats through a bounded hexadecimal and ASCII fallback.
6. Move to the preceding/next regular file in the captured visible directory order.
7. Adjust image viewing with bounded 0.5x–2.0x zoom controls.
8. Copy the currently visible text or hexadecimal report to the system clipboard.
9. Copy the selected file's native path or provider URI to the clipboard.
10. See the filename, content type, logical size and preview availability/status.

A Cancel loading command interrupts the current read, and closing a preview cancels outstanding work. Files with missing size information, over 8 MiB, or unsupported providers are explicitly shown as unavailable rather than scanned without limit.

## Engineering design

`quick_preview.cpp` is the sole owner of the transient preview session, independent from the durable mutation engine. Opening captures a bounded snapshot of at most 4,096 visible regular files and holds explicit GFile references for its lifetime; it never stores pointers into the live directory model. Each read is a GTask with one GCancellable and a monotonically increasing request generation. Results are applied only if their generation matches the active session, preventing cancelled or superseded reads from painting newer preview state. Completion holds a weak GTK object reference so destroyed windows cannot be accessed.

Worker reads use GIO with a verified at-most-8 MiB input policy, catching growth beyond the originally observed size. They do not follow selected final symlinks. Image decoding runs on a worker and scales images to a maximum 1,050 × 740 preview; GTK textures are created only on the main loop. The plain text viewer displays at most 256 KiB of validated UTF-8 text, while other supported bounded regular files expose only the first 256 bytes as hex. Preview results have no write, rename or delete authority. Limits and unavailable states are not hidden behind a pretend preview.

## Qualification and exclusions

The Ubuntu 24.04 hosted CI completed 18/18 CTest application tests, including real GTK interactions for text, image, zoom, hex, clipboard and navigation. The native installer also completed its aggressive installation tests. Release and central beta APT verification are separately required for a green publication verdict.

No full media playback, PDF renderer, large-file streaming, image editing, EXIF editor, binary editor, multi-frame animation, color-managed proofing or filesystem-specific historical previews is implied. Day/Night visual capture on the user's actual Linux Mint desktop is an outstanding acceptance task.
