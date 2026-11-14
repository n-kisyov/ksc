#include "zip_util.h"
#include "ksc_private.h"
#include <stdint.h>
#include <zlib.h>

static constexpr uint32_t ZIP_SIG_LOCAL   = 0x04034B50U;
static constexpr uint32_t ZIP_SIG_CENTRAL = 0x02014B50U;
static constexpr uint32_t ZIP_SIG_EOCD    = 0x06054B50U;
static constexpr uint32_t ZIP_SIG_DDESC   = 0x08074B50U;
static constexpr uint16_t ZIP_METHOD_STORE   = 0;
static constexpr uint16_t ZIP_METHOD_DEFLATE = 8;
static constexpr size_t   ZIP_CHUNK = 32768;

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void get_dos_datetime(uint16_t *dosTime, uint16_t *dosDate)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    int yr = (st.wYear >= 1980) ? (st.wYear - 1980) : 0;
    if (yr > 127) yr = 127;
    *dosTime = (uint16_t)(((st.wHour & 0x1F) << 11) |
                          ((st.wMinute & 0x3F) << 5) |
                          ((st.wSecond / 2) & 0x1F));
    *dosDate = (uint16_t)(((yr & 0x7F) << 9) |
                          ((st.wMonth & 0x0F) << 5) |
                          (st.wDay & 0x1F));
}

bool zip_compress_file(const char *srcPath, const char *zipPath,
                       const char *entryName)
{
    if (!srcPath || !zipPath || !entryName || !entryName[0]) return false;

    size_t nameLen = strlen(entryName);
    if (nameLen > 0xFFFFU) return false;

    FILE *fin = fopen(srcPath, "rb");
    if (!fin) return false;

    FILE *fout = fopen(zipPath, "wb");
    if (!fout) {
        fclose(fin);
        return false;
    }

    uint16_t dosTime = 0, dosDate = 0;
    get_dos_datetime(&dosTime, &dosDate);

    uint8_t lfh[30] = {};
    put_le32(lfh + 0, ZIP_SIG_LOCAL);
    put_le16(lfh + 4, 20);
    put_le16(lfh + 6, 0);
    put_le16(lfh + 8, ZIP_METHOD_DEFLATE);
    put_le16(lfh + 10, dosTime);
    put_le16(lfh + 12, dosDate);
    put_le16(lfh + 26, (uint16_t)nameLen);
    put_le16(lfh + 28, 0);

    bool ok = (fwrite(lfh, 1, sizeof(lfh), fout) == sizeof(lfh)) &&
              (fwrite(entryName, 1, nameLen, fout) == nameLen);
    if (!ok) {
        fclose(fin);
        fclose(fout);
        DeleteFile(zipPath);
        return false;
    }

    z_stream strm = {};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        fclose(fin);
        fclose(fout);
        DeleteFile(zipPath);
        return false;
    }

    uint8_t inBuf[ZIP_CHUNK];
    uint8_t outBuf[ZIP_CHUNK];
    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
    uint64_t uncompTotal = 0;
    uint64_t compTotal = 0;
    int flush = Z_NO_FLUSH;

    do {
        size_t nr = fread(inBuf, 1, sizeof(inBuf), fin);
        if (ferror(fin)) {
            ok = false;
            break;
        }
        flush = feof(fin) ? Z_FINISH : Z_NO_FLUSH;
        if (nr > 0) {
            crc = (uint32_t)crc32(crc, inBuf, (uInt)nr);
            uncompTotal += nr;
        }
        strm.avail_in = (uInt)nr;
        strm.next_in = inBuf;

        do {
            strm.avail_out = (uInt)sizeof(outBuf);
            strm.next_out = outBuf;
            int ret = deflate(&strm, flush);
            if (ret == Z_STREAM_ERROR) {
                ok = false;
                break;
            }
            size_t have = sizeof(outBuf) - strm.avail_out;
            if (have > 0) {
                if (fwrite(outBuf, 1, have, fout) != have) {
                    ok = false;
                    break;
                }
                compTotal += have;
            }
        } while (strm.avail_out == 0 && ok);
    } while (flush != Z_FINISH && ok);

    deflateEnd(&strm);
    fclose(fin);

    if (uncompTotal > 0xFFFFFFFFULL || compTotal > 0xFFFFFFFFULL) {
        ok = false;
    }

    if (ok) {
        long cdOffset = ftell(fout);
        if (cdOffset < 0) ok = false;

        if (ok) {
            put_le32(lfh + 14, crc);
            put_le32(lfh + 18, (uint32_t)compTotal);
            put_le32(lfh + 22, (uint32_t)uncompTotal);

            if (fseek(fout, 0, SEEK_SET) != 0 ||
                fwrite(lfh, 1, sizeof(lfh), fout) != sizeof(lfh) ||
                fseek(fout, cdOffset, SEEK_SET) != 0) {
                ok = false;
            }
        }

        if (ok) {
            uint8_t cdh[46] = {};
            put_le32(cdh + 0, ZIP_SIG_CENTRAL);
            put_le16(cdh + 4, 20);
            put_le16(cdh + 6, 20);
            put_le16(cdh + 8, 0);
            put_le16(cdh + 10, ZIP_METHOD_DEFLATE);
            put_le16(cdh + 12, dosTime);
            put_le16(cdh + 14, dosDate);
            put_le32(cdh + 16, crc);
            put_le32(cdh + 20, (uint32_t)compTotal);
            put_le32(cdh + 24, (uint32_t)uncompTotal);
            put_le16(cdh + 28, (uint16_t)nameLen);
            put_le16(cdh + 30, 0);
            put_le16(cdh + 32, 0);
            put_le16(cdh + 34, 0);
            put_le16(cdh + 36, 0);
            put_le32(cdh + 38, 0);
            put_le32(cdh + 42, 0);

            uint32_t cdSize = (uint32_t)(sizeof(cdh) + nameLen);
            uint8_t eocd[22] = {};
            put_le32(eocd + 0, ZIP_SIG_EOCD);
            put_le16(eocd + 4, 0);
            put_le16(eocd + 6, 0);
            put_le16(eocd + 8, 1);
            put_le16(eocd + 10, 1);
            put_le32(eocd + 12, cdSize);
            put_le32(eocd + 16, (uint32_t)cdOffset);
            put_le16(eocd + 20, 0);

            if (fwrite(cdh, 1, sizeof(cdh), fout) != sizeof(cdh) ||
                fwrite(entryName, 1, nameLen, fout) != nameLen ||
                fwrite(eocd, 1, sizeof(eocd), fout) != sizeof(eocd)) {
                ok = false;
            }
        }
    }

    if (fflush(fout) != 0) ok = false;
    fclose(fout);
    if (!ok) {
        DeleteFile(zipPath);
    }
    return ok;
}

bool zip_extract_first_file(const char *zipPath, const char *destPath)
{
    if (!zipPath || !destPath) return false;

    FILE *fin = fopen(zipPath, "rb");
    if (!fin) return false;

    uint8_t lfh[30];
    if (fread(lfh, 1, sizeof(lfh), fin) != sizeof(lfh) ||
        get_le32(lfh) != ZIP_SIG_LOCAL) {
        fclose(fin);
        return false;
    }

    uint16_t flags       = get_le16(lfh + 6);
    uint16_t method      = get_le16(lfh + 8);
    uint32_t expectedCrc = get_le32(lfh + 14);
    uint32_t compSize    = get_le32(lfh + 18);
    uint32_t uncompSize  = get_le32(lfh + 22);
    uint16_t nameLen     = get_le16(lfh + 26);
    uint16_t extraLen    = get_le16(lfh + 28);

    if (flags & 0x0001U) {
        /* Encrypted zip unsupported */
        fclose(fin);
        return false;
    }

    if (fseek(fin, (long)nameLen + (long)extraLen, SEEK_CUR) != 0) {
        fclose(fin);
        return false;
    }

    FILE *fout = fopen(destPath, "wb");
    if (!fout) {
        fclose(fin);
        return false;
    }

    bool ok = false;
    uint32_t actualCrc = (uint32_t)crc32(0L, Z_NULL, 0);
    uint64_t actualOut = 0;

    if (method == ZIP_METHOD_STORE) {
        ok = true;
        uint8_t buf[ZIP_CHUNK];
        uint64_t rem = compSize;
        while (rem > 0) {
            size_t toRead = (rem > sizeof(buf)) ? sizeof(buf) : (size_t)rem;
            size_t nr = fread(buf, 1, toRead, fin);
            if (nr == 0) {
                ok = false;
                break;
            }
            if (fwrite(buf, 1, nr, fout) != nr) {
                ok = false;
                break;
            }
            actualCrc = (uint32_t)crc32(actualCrc, buf, (uInt)nr);
            actualOut += nr;
            rem -= nr;
        }
        if (ok && (actualOut != uncompSize || actualCrc != expectedCrc)) {
            ok = false;
        }
    } else if (method == ZIP_METHOD_DEFLATE) {
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) == Z_OK) {
            uint8_t inBuf[ZIP_CHUNK];
            uint8_t outBuf[ZIP_CHUNK];
            int ret = Z_OK;
            ok = true;

            do {
                size_t nr = fread(inBuf, 1, sizeof(inBuf), fin);
                if (nr == 0) {
                    if (ferror(fin)) ok = false;
                    break;
                }
                strm.avail_in = (uInt)nr;
                strm.next_in = inBuf;

                do {
                    strm.avail_out = (uInt)sizeof(outBuf);
                    strm.next_out = outBuf;
                    ret = inflate(&strm, Z_NO_FLUSH);
                    if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR ||
                        ret == Z_MEM_ERROR || ret == Z_STREAM_ERROR) {
                        ok = false;
                        break;
                    }
                    size_t have = sizeof(outBuf) - strm.avail_out;
                    if (have > 0) {
                        if (fwrite(outBuf, 1, have, fout) != have) {
                            ok = false;
                            break;
                        }
                        actualCrc = (uint32_t)crc32(actualCrc, outBuf, (uInt)have);
                        actualOut += have;
                    }
                } while (strm.avail_out == 0 && ok && ret != Z_STREAM_END);
            } while (ok && ret != Z_STREAM_END);

            if (ok && ret != Z_STREAM_END) {
                ok = false;
            }
            /* If data descriptor is present, read trailing CRC/sizes from it */
            if (ok && (flags & 0x0008U)) {
                long afterStream = ftell(fin) - (long)strm.avail_in;
                if (afterStream >= 0 && fseek(fin, afterStream, SEEK_SET) == 0) {
                    uint8_t dd[16];
                    size_t nrd = fread(dd, 1, 16, fin);
                    if (nrd >= 12) {
                        size_t off = (get_le32(dd) == ZIP_SIG_DDESC && nrd == 16) ? 4 : 0;
                        expectedCrc = get_le32(dd + off);
                        uncompSize  = get_le32(dd + off + 8);
                    }
                }
            }
            if (ok && (actualCrc != expectedCrc || actualOut != uncompSize)) {
                ok = false;
            }
            inflateEnd(&strm);
        }
    }

    if (fflush(fout) != 0) ok = false;
    fclose(fout);
    fclose(fin);

    if (!ok) {
        DeleteFile(destPath);
    }
    return ok;
}
