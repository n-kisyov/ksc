#ifndef ZIP_UTIL_H
#define ZIP_UTIL_H

[[nodiscard]] bool zip_compress_file(const char *srcPath, const char *zipPath,
                                     const char *entryName);
[[nodiscard]] bool zip_extract_first_file(const char *zipPath,
                                          const char *destPath);

#endif
