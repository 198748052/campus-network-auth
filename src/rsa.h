/* rsa.h - RSA 加密（Windows CNG/BCrypt 实现）
 *
 * 对应 Python 版 eportal/rsa_util.py 与登录页 security.js 逻辑：
 *   1. 密码字符串倒序（仅反转密码本身）
 *   2. pwdMac = 密码 + ">" + "111111111"，长度 >= 150 时前端不加密，明文提交
 *   3. 用服务端下发的 RSA 公钥 (modulus, exponent) 做 PKCS#1 v1.5 加密
 *   4. 密文以 16 进制小写字符串提交
 * 依赖系统自带 bcrypt.dll（CNG），零第三方依赖。
 */
#ifndef RSA_H
#define RSA_H

#ifdef __cplusplus
extern "C" {
#endif

/* 加密密码，返回 malloc 的 16 进制密文（小写）；
 * 密码超长（>=150，与前端一致）或失败时返回 NULL。
 * modulus_hex / exponent_hex 为服务端 pageInfo 下发的 16 进制字符串。 */
char *rsa_encrypt_password(const char *password, const char *modulus_hex,
                           const char *exponent_hex);

#ifdef __cplusplus
}
#endif

#endif /* RSA_H */
