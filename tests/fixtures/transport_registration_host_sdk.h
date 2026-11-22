// Native adapters for the full registration transport unit. The wire fixture
// uses numeric loopback addresses, so asynchronous DNS is never requested.
#include <openssl/evp.h>
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *value) {
  value->given=true;return value;
}
using err_t=int;
using ip_addr_t=in_addr;
constexpr err_t ERR_OK=0,ERR_INPROGRESS=-1,ERR_VAL=-2;
constexpr int LWIP_DNS_ADDRTYPE_IPV4=0;
#define IP_IS_V4(address) true
#define ip_2_ip4(address) (address)
#define ip4_addr_get_u32(address) ((address)->s_addr)
inline err_t dns_gethostbyname_addrtype(const char*,ip_addr_t*,void(*)(const char*,const ip_addr_t*,void*),void*,int) {std::abort();}
inline err_t tcpip_try_callback(void(*)(void*),void*) {std::abort();}
using mbedtls_md_info_t=EVP_MD;
constexpr int MBEDTLS_MD_NONE=0,MBEDTLS_MD_MD5=1,MBEDTLS_MD_SHA256=2,MBEDTLS_MD_MAX_SIZE=EVP_MAX_MD_SIZE;
inline const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) {
  return type==MBEDTLS_MD_MD5?EVP_md5():type==MBEDTLS_MD_SHA256?EVP_sha256():nullptr;
}
inline size_t mbedtls_md_get_size(const mbedtls_md_info_t *info) {return EVP_MD_get_size(info);}
inline int mbedtls_md(const mbedtls_md_info_t *info,const unsigned char *input,size_t size,unsigned char *out) {
  unsigned length=0;return EVP_Digest(input,size,out,&length,info,nullptr)==1?0:-1;
}
