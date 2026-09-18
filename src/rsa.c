/* rsa.c - RSA 加密实现（BCrypt/CNG） */
#include "rsa.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "util.h"

#pragma comment(lib, "bcrypt.lib")

/* 部分 Windows SDK 的 bcrypt.h 未定义以下常量（仅在注释中给出），这里补全：
 * - BCRYPT_RSAPUBLIC_KEY_BLOB：导入 RSA 公钥时的对象标识符（魔术串）
 * - BCRYPT_PKCS1_PADDING：BCryptEncrypt 的 PKCS#1 v1.5 填充方案
 */
#ifndef BCRYPT_RSAPUBLIC_KEY_BLOB
#define BCRYPT_RSAPUBLIC_KEY_BLOB L"RSAPUBLICBLOB"
#endif
/* 极旧 SDK 若未定义 BCRYPT_PAD_PKCS1（0x00000002，PKCS#1 v1.5 填充），补一个默认值 */
#ifndef BCRYPT_PAD_PKCS1
#define BCRYPT_PAD_PKCS1 0x00000002
#endif
/* BCRYPT_RSAKEY_BLOB 内存结构 Magic 字段（"RSA1"，小端 ASCII） */
#ifndef BCRYPT_RSAPUBLICKEY_MAGIC
#define BCRYPT_RSAPUBLICKEY_MAGIC 0x31415352
#endif

/* 拼接密码的默认 MAC（对应 security.js mac 参数缺省值 111111111）。
 * 该 MAC 仅参与"是否加密"的长度判断，不参与加密内容。 */
#define DEFAULT_MAC L"111111111"

/* 把大端序 hex 字符串解码为字节数组（去除前导 0，保持大端序）。
 * 兼容奇数长度输入：如指数 "10001"（5 位）先补前导 '0' 成 "010001" 再解码。
 * CNG 的 BCRYPT_RSAKEY_BLOB 中 exponent 与 modulus 均按大端序存放，
 * 与 .NET ExportParameters 及服务端 pageInfo 下发的格式一致，无需反转。
 * 返回 malloc 字节数组，长度写 *out_len；失败返回 NULL。 */
static unsigned char *big_endian_hex_to_bytes(const char *hex, size_t *out_len)
{
    if (!hex)
        return NULL;
    size_t hexlen = strlen(hex);
    if (hexlen == 0)
        return NULL;

    /* 奇数长度：补一个前导 '0'（如 "10001" -> "010001"），避免半字节无法解析 */
    char *padded = NULL;
    if (hexlen % 2 != 0) {
        padded = (char *)malloc(hexlen + 2);
        if (!padded)
            return NULL;
        padded[0] = '0';
        memcpy(padded + 1, hex, hexlen + 1);
        hex = padded;
        hexlen++;
    }
    size_t n = hexlen / 2;
    unsigned char *be = (unsigned char *)malloc(n ? n : 1);
    if (!be) {
        free(padded);
        return NULL;
    }
    if (hex_decode(hex, be, n) < 0) {
        free(be);
        free(padded);
        return NULL;
    }
    free(padded);
    /* 去除大端前导 0 */
    size_t start = 0;
    while (start < n && be[start] == 0)
        start++;
    size_t m = n - start;
    if (m == 0) {
        free(be);
        *out_len = 0;
        unsigned char *empty = (unsigned char *)malloc(1);
        if (empty)
            empty[0] = 0;
        return empty;
    }
    if (start > 0)
        memmove(be, be + start, m); /* 前移去掉前导 0，保持大端序 */
    *out_len = m;
    return be;
}

/* 由大端序 modulus 字节数组计算精确位长（去除前导 0 字节后首个字节的有效位数） */
static ULONG modulus_bit_length(const unsigned char *mod, size_t mod_len)
{
    if (mod_len == 0)
        return 0;
    unsigned char b = mod[0];
    int bits = 8;
    while (bits > 0 && (b & 0x80) == 0) {
        b <<= 1;
        bits--;
    }
    return (ULONG)((mod_len - 1) * 8 + bits);
}

char *rsa_encrypt_password(const char *password, const char *modulus_hex,
                           const char *exponent_hex)
{
    if (!password)
        return NULL;

    /* 与前端 security.js 一致：先按 UTF-16 码元反转（pwd.split("").reverse().join("")），
     * 中文等多字节字符才不会因"按字节反转"而乱码（原 Python 版按字符反转，行为相同） */
    wchar_t *wide = utf8_to_wide(password);
    if (!wide)
        return NULL;
    size_t wlen = wcslen(wide);

    /* 边界防御（与 security.js 一致）：pwdMac 长度 >= 150 时不加密，明文提交。
     * 长度按 UTF-16 码元统计，与前端 JS 的 .length 一致。 */
    if (wlen + 1 + wcslen(DEFAULT_MAC) >= 150) {
        free(wide);
        return NULL; /* 调用方此时应直接提交明文 */
    }
    if (!modulus_hex || !*modulus_hex) {
        free(wide);
        return NULL;
    }

    if (!exponent_hex || !*exponent_hex)
        exponent_hex = "10001";

    /* 1. 密码倒序（按 UTF-16 码元反转，再转回 UTF-8 字节流参与加密） */
    for (size_t i = 0; i < wlen / 2; i++) {
        wchar_t t = wide[i];
        wide[i] = wide[wlen - 1 - i];
        wide[wlen - 1 - i] = t;
    }
    char *rev = wide_to_utf8(wide);
    free(wide);
    if (!rev)
        return NULL;
    size_t pwdlen = strlen(rev);

    /* 2. 解析公钥参数为大端字节（BCRYPT_RSAKEY_BLOB 中 exponent/modulus 均为大端序） */
    size_t exp_len = 0, mod_len = 0;
    unsigned char *exp_be = big_endian_hex_to_bytes(exponent_hex, &exp_len);
    unsigned char *mod_be = big_endian_hex_to_bytes(modulus_hex, &mod_len);
    if (!exp_be || !mod_be || exp_len == 0 || mod_len == 0) {
        free(rev);
        free(exp_be);
        free(mod_be);
        return NULL;
    }

    /* 3. 构造 BCRYPT_RSAKEY_BLOB（RSAPUBLICKEYBLOB 结构） */
    size_t bloblen = 6 * sizeof(ULONG) + exp_len + mod_len;
    unsigned char *blob = (unsigned char *)calloc(1, bloblen);
    if (!blob) {
        free(rev);
        free(exp_be);
        free(mod_be);
        return NULL;
    }
    ULONG *hdr = (ULONG *)blob;
    hdr[0] = BCRYPT_RSAPUBLICKEY_MAGIC; /* Magic：0x31415352 "RSA1" */
    hdr[1] = (ULONG)(mod_len * 8);      /* BitLength */
    hdr[2] = (ULONG)exp_len;            /* cbPublicExp */
    hdr[3] = (ULONG)mod_len;            /* cbModulus */
    hdr[4] = 0;                         /* cbPrime1 */
    hdr[5] = 0;                         /* cbPrime2 */
    memcpy(blob + 6 * sizeof(ULONG), exp_be, exp_len);
    memcpy(blob + 6 * sizeof(ULONG) + exp_len, mod_be, mod_len);

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    NTSTATUS st;
    char *out_hex = NULL;

    st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, NULL, 0);
    if (st != 0)
        goto done;

    st = BCryptImportKeyPair(hAlg, NULL, BCRYPT_RSAPUBLIC_KEY_BLOB, &hKey,
                             (PUCHAR)blob, (ULONG)bloblen, 0);
    if (st != 0)
        goto done;

    /* 4. PKCS#1 v1.5 加密（BCRYPT_PAD_PKCS1）：先查密文长度再执行。
     *    与 Python 版 rsa_util.py 一致：加密内容仅为倒序后的密码本身，
     *    DEFAULT_MAC 只参与上方"是否加密"的长度判断，不参与加密内容。 */
    ULONG cbOut = 0;
    st = BCryptEncrypt(hKey, (PUCHAR)rev, (ULONG)pwdlen, NULL, NULL, 0,
                       NULL, 0, &cbOut, BCRYPT_PAD_PKCS1);
    if (st != 0)
        goto done;
    unsigned char *cipher = (unsigned char *)malloc(cbOut);
    if (!cipher)
        goto done;
    st = BCryptEncrypt(hKey, (PUCHAR)rev, (ULONG)pwdlen, NULL, NULL, 0,
                       cipher, cbOut, &cbOut, BCRYPT_PAD_PKCS1);
    if (st == 0) {
        out_hex = hex_encode(cipher, cbOut);
    }
    free(cipher);

done:
    if (hKey)
        BCryptDestroyKey(hKey);
    if (hAlg)
        BCryptCloseAlgorithmProvider(hAlg, 0);
    free(rev);
    free(exp_be);
    free(mod_be);
    free(blob);
    return out_hex;
}
