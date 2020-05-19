/*
 * Copyright (c) 2018 ARM Limited
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TLSSocketWrapper.h"
#include "platform/Callback.h"
#include "drivers/Timer.h"
#include "events/mbed_events.h"

#define TRACE_GROUP "TLSW"
#include "mbed-trace/mbed_trace.h"

#ifdef USE_WOLFSSL_LIB
#include "wolfssl/wolfcrypt/settings.h"
#include "wolfssl/ssl.h"
#include "wolfssl/internal.h" /* to allow dereferencing WOLFSSL_X509 */
#include "wolfssl/wolfcrypt/asn.h"
#else
#include "mbedtls/debug.h"
#include "mbedtls/platform.h"
#include "mbed_error.h"
#endif

#include "Kernel.h"

/* ------------------------------------------------------------------ */
/* wolfSSL Wrapper */
/* ------------------------------------------------------------------ */
#ifdef USE_WOLFSSL_LIB

TLSSocketWrapper::TLSSocketWrapper(Socket *transport, const char *hostname, control_transport control) :
    _ssl(NULL),
    _transport(transport),
    _timeout(-1),
    _ssl_conf(NULL),
    _cacert(NULL),
    _clicert(NULL),
    _connect_transport(control == TRANSPORT_CONNECT || control == TRANSPORT_CONNECT_AND_CLOSE),
    _close_transport(control == TRANSPORT_CLOSE || control == TRANSPORT_CONNECT_AND_CLOSE),
    _tls_initialized(false),
    _handshake_completed(false),
    _cacert_allocated(false),
    _clicert_allocated(false),
    _ssl_conf_allocated(false)
{
    wolfSSL_Init();
#ifdef DEBUG_WOLFSSL
    wolfSSL_SetLoggingCb(my_debug);
    wolfSSL_Debugging_ON();
#endif
    if (hostname) {
        set_hostname(hostname);
    }
}

TLSSocketWrapper::~TLSSocketWrapper()
{
    if (_transport) {
        close();
    }
    if (_ssl) {
        wolfSSL_free(_ssl);
        _ssl = NULL;
    }
    if (_ssl_conf && _ssl_conf_allocated) {
        wolfSSL_CTX_free(_ssl_conf);
        _ssl_conf = NULL;
    }
    wolfSSL_Cleanup();
}

void TLSSocketWrapper::set_hostname(const char *hostname)
{
#ifdef HAVE_SNI
    wolfSSL_UseSNI(_ssl, 0, hostname, (word16)XSTRLEN(hostname));
#endif
}

nsapi_error_t TLSSocketWrapper::set_root_ca_cert(const void *root_ca, size_t len)
{
    int ret;
    WOLFSSL_X509* crt;
    DerBuffer* der;

    ret = wc_PemToDer(static_cast<const unsigned char *>(root_ca), 
        len, CA_TYPE, &der, NULL, NULL, NULL);
    if (ret < 0) {
        print_mbedtls_error("set_root_ca_cert: wc_PemToDer", ret);
        wc_FreeDer(&der);
        return NSAPI_ERROR_PARAMETER;
    }

    crt = wolfSSL_X509_d2i(NULL, der->buffer, der->length);
    wc_FreeDer(&der);
    if (crt == NULL) {
        print_mbedtls_error("set_root_ca_cert: wolfSSL_X509_d2i error", 0);
        return NSAPI_ERROR_PARAMETER;
    }

    set_ca_chain(crt);
    _cacert_allocated = true;
    return NSAPI_ERROR_OK;
}

nsapi_error_t TLSSocketWrapper::set_root_ca_cert(const char *root_ca_pem)
{
    return set_root_ca_cert(root_ca_pem, strlen(root_ca_pem) + 1);
}

nsapi_error_t TLSSocketWrapper::set_client_cert_key(const char *client_cert_pem, const char *client_private_key_pem)
{
    return set_client_cert_key(client_cert_pem, strlen(client_cert_pem) + 1, client_private_key_pem, strlen(client_private_key_pem) + 1);
}

nsapi_error_t TLSSocketWrapper::set_client_cert_key(const void *client_cert, size_t client_cert_len,
                                                    const void *client_private_key_pem, size_t client_private_key_len)
{
    int ret;
    WOLFSSL_X509* crt;
    DerBuffer* der;

    ret = wolfSSL_CTX_use_PrivateKey_buffer(get_ssl_config(), 
        static_cast<const unsigned char *>(client_private_key_pem), client_private_key_len,
        WOLFSSL_FILETYPE_PEM);
    if (ret != WOLFSSL_SUCCESS) {
        print_mbedtls_error("wolfSSL_use_PrivateKey_buffer", ret);
        return NSAPI_ERROR_PARAMETER;
    }

    ret = wc_PemToDer(static_cast<const unsigned char *>(client_cert), 
        client_cert_len, CERT_TYPE, &der, NULL, NULL, NULL);
    if (ret < 0) {
        print_mbedtls_error("wc_PemToDer", ret);
        wc_FreeDer(&der);
        return NSAPI_ERROR_PARAMETER;
    }

    crt = wolfSSL_X509_d2i(NULL, der->buffer, der->length);
    wc_FreeDer(&der);
    if (crt == NULL) {
        print_mbedtls_error("wolfSSL_X509_d2i error", 0);
        return NSAPI_ERROR_PARAMETER;
    }

    set_own_cert(crt);
    _clicert_allocated = true;

    return NSAPI_ERROR_OK;
}


nsapi_error_t TLSSocketWrapper::start_handshake(bool first_call)
{
    int ret;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (_tls_initialized) {
        return continue_handshake();
    }

    tr_info("Starting TLS handshake");

    _ssl = wolfSSL_new(get_ssl_config());
    if (_ssl == NULL) {
        return NSAPI_ERROR_AUTH_FAILURE;
    }

    _transport->set_blocking(false);
    _transport->sigio(mbed::callback(this, &TLSSocketWrapper::event));

    wolfSSL_SSLSetIORecv(_ssl, ssl_recv);
    wolfSSL_SSLSetIOSend(_ssl, ssl_send);
    wolfSSL_SetIOReadCtx(_ssl, (void*)this);
    wolfSSL_SetIOWriteCtx(_ssl, (void*)this);

    _tls_initialized = true;

    ret = continue_handshake();
    if (first_call) {
        if (ret == NSAPI_ERROR_ALREADY) {
            ret = NSAPI_ERROR_IN_PROGRESS; // If first call should return IN_PROGRESS
        }
        if (ret == NSAPI_ERROR_IS_CONNECTED) {
            ret = NSAPI_ERROR_OK;   // If we happened to complete the request on the first call, return OK.
        }
    }
    return ret;
}

nsapi_error_t TLSSocketWrapper::continue_handshake()
{
    int ret, err = 0;

    if (_handshake_completed) {
        return NSAPI_ERROR_IS_CONNECTED;
    }

    if (!_tls_initialized) {
        return NSAPI_ERROR_NO_CONNECTION;
    }

    while (true) {
        ret = wolfSSL_connect(_ssl);
        err = wolfSSL_get_error(_ssl, ret);
        if (_timeout && (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE)) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                break;
            }
        } else {
            break;
        }
    }

    if (ret < 0) {
        print_mbedtls_error("wolfSSL_connect", err);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            return NSAPI_ERROR_ALREADY;
        } else {
            return NSAPI_ERROR_AUTH_FAILURE;
        }
    }

    /* It also means the handshake is done, time to print info */
    tr_info("TLS connection established");

    _handshake_completed = true;
    return NSAPI_ERROR_IS_CONNECTED;
}


nsapi_error_t TLSSocketWrapper::send(const void *data, nsapi_size_t size)
{
    int ret, err = 0;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    tr_debug("send %d", size);
    while (true) {
        if (!_handshake_completed) {
            ret = continue_handshake();
            if (ret != NSAPI_ERROR_IS_CONNECTED) {
                if (ret == NSAPI_ERROR_ALREADY) {
                    ret = NSAPI_ERROR_WOULD_BLOCK;
                }
                return ret;
            }
        }

        ret = wolfSSL_write(_ssl, (const unsigned char *) data, size);
        err = wolfSSL_get_error(_ssl, ret);

        if (_timeout == 0) {
            break;
        } else if (err == WOLFSSL_ERROR_WANT_WRITE || 
                   err == WOLFSSL_ERROR_WANT_READ) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                // Timeout break
                break;
            }
        } else {
            break;
        }
    }

    if (err == WOLFSSL_ERROR_WANT_WRITE ||
            err == WOLFSSL_ERROR_WANT_READ) {
        // translate to socket error
        return NSAPI_ERROR_WOULD_BLOCK;
    }

    if (ret < 0) {
        print_mbedtls_error("wolfSSL_write", err);
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return ret; // Assume "non negative errorcode" to be propagated from Socket layer
}

nsapi_size_or_error_t TLSSocketWrapper::sendto(const SocketAddress &, const void *data, nsapi_size_t size)
{
    // Ignore the SocketAddress
    return send(data, size);
}

nsapi_size_or_error_t TLSSocketWrapper::recv(void *data, nsapi_size_t size)
{
    int ret, err = 0;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    while (true) {
        if (!_handshake_completed) {
            ret = continue_handshake();
            if (ret != NSAPI_ERROR_IS_CONNECTED) {
                if (ret == NSAPI_ERROR_ALREADY) {
                    ret = NSAPI_ERROR_WOULD_BLOCK;
                }
                return ret;
            }
        }

        ret = wolfSSL_read(_ssl, (unsigned char *) data, size);
        err = wolfSSL_get_error(_ssl, ret);
        if (_timeout == 0) {
            break;
        } else if (err == WOLFSSL_ERROR_WANT_WRITE || err == WOLFSSL_ERROR_WANT_READ) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                // Timeout break
                break;
            }
        } else {
            break;
        }
    }
    if (err == WOLFSSL_ERROR_WANT_WRITE ||
            err == WOLFSSL_ERROR_WANT_READ) {
        // translate to socket error
        return NSAPI_ERROR_WOULD_BLOCK;
    } else if (ret == WOLFSSL_ERROR_ZERO_RETURN) {
        /* WOLFSSL_ERROR_ZERO_RETURN is not considered as error.
         * Just ignore here. Once connection is closed, wolfSSL_read()
         * will return 0.
         */
        return 0;
    } else if (ret < 0) {
        print_mbedtls_error("wolfSSL_read", err);
        // There is no mapping of TLS error codes to Socket API so return most generic error to application
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return ret;
}

nsapi_size_or_error_t TLSSocketWrapper::recvfrom(SocketAddress *address, void *data, nsapi_size_t size)
{
    if (address) {
        getpeername(address);
    }
    return recv(data, size);
}

void TLSSocketWrapper::print_mbedtls_error(MBED_UNUSED const char *name, MBED_UNUSED int err)
{
    tr_err("%s() failed: %d: %s", name, err, wolfSSL_ERR_reason_error_string(err));
}

#ifdef DEBUG_WOLFSSL
void TLSSocketWrapper::my_debug(const int logLevel, const char *const logMessage)
{
    printf("[TLS %d] %s\r\n", logLevel, logMessage);
}
#endif /* DEBUG_WOLFSSL */

int TLSSocketWrapper::my_verify(int preverify, WOLFSSL_X509_STORE_CTX* store)
{
    /* Verify Callback Arguments:
     * preverify:           1=Verify Okay, 0=Failure
     * store->error:        Failure error code (0 indicates no failure)
     * store->current_cert: Current WOLFSSL_X509 object (only with OPENSSL_EXTRA)
     * store->error_depth:  Current Index
     * store->domain:       Subject CN as string (null term)
     * store->totalCerts:   Number of certs presented by peer
     * store->certs[i]:     A `WOLFSSL_BUFFER_INFO` with plain DER for each cert
     * store->store:        WOLFSSL_X509_STORE with CA cert chain
     * store->store->cm:    WOLFSSL_CERT_MANAGER
     * store->ex_data:      The WOLFSSL object pointer
     * store->discardSessionCerts: When set to non-zero value session certs
        will be discarded (only with SESSION_CERTS)
     */

    printf("In verification callback, error = %d, %s\r\n", store->error,
        wolfSSL_ERR_reason_error_string(store->error));

#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
    if (store->current_cert) {
        WOLFSSL_X509* peer = store->current_cert;
        char* issuer  = wolfSSL_X509_NAME_oneline(
                                       wolfSSL_X509_get_issuer_name(peer), 0, 0);
        char* subject = wolfSSL_X509_NAME_oneline(
                                      wolfSSL_X509_get_subject_name(peer), 0, 0);
        printf("\tPeer's cert info:\n issuer : %s\n subject: %s\r\n", issuer,
                                                                  subject);
        XFREE(subject, 0, DYNAMIC_TYPE_OPENSSL);
        XFREE(issuer,  0, DYNAMIC_TYPE_OPENSSL);
    }
    else {
        printf("\tPeer has no cert!\r\n");
    }
#else
    printf("\tPeer certs: %d\r\n", store->totalCerts);
    #ifdef SHOW_CERTS
    {   int i;
        for (i=0; i<store->totalCerts; i++) {
            WOLFSSL_BUFFER_INFO* cert = &store->certs[i];
            printf("\t\tCert %d: Ptr %p, Len %u\r\n", i, cert->buffer, cert->length);
        }
    }
    #endif /* SHOW_CERTS */
#endif /* OPENSSL_EXTRA || OPENSSL_EXTRA_X509_SMALL */

    printf("\tSubject's domain name at %d is %s\r\n", store->error_depth, store->domain);

    /* allow date error on certificate as RTC may not be set */
    if (store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E) {
        printf("Overriding cert date error (bad clock?)\r\n");
        preverify = 1; /* override any date error */
    }

    /* A non-zero return code indicates failure override */
    return preverify;
}

int TLSSocketWrapper::ssl_recv(WOLFSSL *ssl, char *buf, int len, void *ctx)
{
    int recv;
    TLSSocketWrapper *my = static_cast<TLSSocketWrapper *>(ctx);

    if (!my->_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    recv = my->_transport->recv(buf, len);

    if (NSAPI_ERROR_WOULD_BLOCK == recv) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    } else if (recv < 0) {
        tr_error("Socket recv error %d", recv);
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    // Propagate also Socket errors to SSL, it allows negative error codes to be returned here.
    return recv;
}

int TLSSocketWrapper::ssl_send(WOLFSSL *ssl, char *buf, int len, void *ctx)
{
    int size = -1;
    TLSSocketWrapper *my = static_cast<TLSSocketWrapper *>(ctx);

    if (!my->_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    size = my->_transport->send(buf, len);

    if (NSAPI_ERROR_WOULD_BLOCK == size) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    } else if (size < 0) {
        tr_error("Socket send error %d", size);
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    // Propagate also Socket errors to SSL, it allows negative error codes to be returned here.
    return size;
}

WOLFSSL_X509 *TLSSocketWrapper::get_own_cert()
{
    return _clicert;
}

int TLSSocketWrapper::set_own_cert(WOLFSSL_X509 *crt)
{
    int ret = 0;
    if (_clicert && _clicert_allocated) {
        wolfSSL_X509_free(_clicert);
        _clicert_allocated = false;
    }
    _clicert = crt;
    if (crt && crt->derCert) {
        int derSz = (int)crt->derCert->length;
        const byte* der = crt->derCert->buffer;

        ret = wolfSSL_CTX_use_certificate_buffer(get_ssl_config(), der, derSz, 
            WOLFSSL_FILETYPE_ASN1);
        if (ret != 0) {
            print_mbedtls_error("wolfSSL_CTX_use_certificate_buffer", ret);
        }
    }
    return ret;
}

WOLFSSL_X509 *TLSSocketWrapper::get_ca_chain()
{
    return _cacert;
}

void TLSSocketWrapper::set_ca_chain(WOLFSSL_X509 *crt)
{
    int ret = 0;
    if (_cacert && _cacert_allocated) {
        wolfSSL_X509_free(_cacert);
        _cacert_allocated = false;
    }
    _cacert = crt;
    if (crt && crt->derCert) {
        int derSz = (int)crt->derCert->length;
        const byte* der = crt->derCert->buffer;

        /* allow date error on trusted CA loading - allow chain */
        ret = wolfSSL_CTX_load_verify_buffer_ex(get_ssl_config(), der, derSz, 
            WOLFSSL_FILETYPE_ASN1, 1, WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY);
        if (ret != 0) {
            print_mbedtls_error("wolfSSL_CTX_load_verify_buffer_ex", ret);
        }
    }
}

WOLFSSL_CTX *TLSSocketWrapper::get_ssl_config()
{
    if (!_ssl_conf) {
        _ssl_conf = wolfSSL_CTX_new(wolfSSLv23_client_method());
        _ssl_conf_allocated = true;

        /* It is possible to disable authentication by passing WOLFSSL_VERIFY_NONE */
        wolfSSL_CTX_set_verify(_ssl_conf, WOLFSSL_VERIFY_PEER, my_verify);
    }
    return _ssl_conf;
}

void TLSSocketWrapper::set_ssl_config(WOLFSSL_CTX *conf)
{
    if (_ssl_conf && _ssl_conf_allocated) {
        wolfSSL_CTX_free(_ssl_conf);
        _ssl_conf = NULL;
        _ssl_conf_allocated = false;
    }
    _ssl_conf = conf;
}

WOLFSSL *TLSSocketWrapper::get_ssl_context()
{
    return _ssl;
}

nsapi_error_t TLSSocketWrapper::close()
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    tr_info("Closing TLS");

    int ret = 0;
    if (_handshake_completed) {
        _transport->set_blocking(true);
        ret = wolfSSL_shutdown(_ssl);
        if (ret) {
            ret = wolfSSL_get_error(_ssl, ret);
            print_mbedtls_error("wolfSSL_shutdown", ret);
        }
        _handshake_completed = false;
    }

    if (_close_transport) {
        int ret2 = _transport->close();
        if (!ret) {
            ret = ret2;
        }
    }

    _transport = NULL;

    return ret;
}

nsapi_error_t TLSSocketWrapper::connect(const SocketAddress &address)
{
    nsapi_error_t ret = NSAPI_ERROR_OK;
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (!is_handshake_started() && _connect_transport) {
        ret = _transport->connect(address);
        if (ret && ret != NSAPI_ERROR_IS_CONNECTED) {
            return ret;
        }
    }
    return start_handshake(ret == NSAPI_ERROR_OK);
}

nsapi_error_t TLSSocketWrapper::bind(const SocketAddress &address)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->bind(address);
}

void TLSSocketWrapper::set_blocking(bool blocking)
{
    set_timeout(blocking ? -1 : 0);
}

void TLSSocketWrapper::set_timeout(int timeout)
{
    _timeout = timeout;
    if (!is_handshake_started() && timeout != -1 && _connect_transport) {
        // If we have not yet connected the transport, we need to modify its blocking mode as well.
        // After connection is initiated, it is already set to non blocking mode
        _transport->set_timeout(timeout);
    }
}

void TLSSocketWrapper::sigio(mbed::Callback<void()> func)
{
    if (!_transport) {
        return;
    }
    _sigio = func;
    _transport->sigio(mbed::callback(this, &TLSSocketWrapper::event));
}

nsapi_error_t TLSSocketWrapper::setsockopt(int level, int optname, const void *optval, unsigned optlen)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->setsockopt(level, optname, optval, optlen);
}

nsapi_error_t TLSSocketWrapper::getsockopt(int level, int optname, void *optval, unsigned *optlen)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->getsockopt(level, optname, optval, optlen);
}

Socket *TLSSocketWrapper::accept(nsapi_error_t *err)
{
    if (err) {
        *err = NSAPI_ERROR_UNSUPPORTED;
    }
    return NULL;
}

nsapi_error_t TLSSocketWrapper::listen(int)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

void TLSSocketWrapper::event()
{
    _event_flag.set(1);
    if (_sigio) {
        _sigio();
    }
}

bool TLSSocketWrapper::is_handshake_started() const
{
    return _tls_initialized;
}


nsapi_error_t TLSSocketWrapper::getpeername(SocketAddress *address)
{
    if (!_handshake_completed) {
        return NSAPI_ERROR_NO_CONNECTION;
    }
    return _transport->getpeername(address);
}


/* ------------------------------------------------------------------ */
/* mbedTLS Wrapper */
/* ------------------------------------------------------------------ */
#elif defined(MBEDTLS_SSL_CLI_C)

TLSSocketWrapper::TLSSocketWrapper(Socket *transport, const char *hostname, control_transport control) :
    _transport(transport),
    _timeout(-1),
#ifdef MBEDTLS_X509_CRT_PARSE_C
    _cacert(NULL),
    _clicert(NULL),
#endif
    _ssl_conf(NULL),
    _connect_transport(control == TRANSPORT_CONNECT || control == TRANSPORT_CONNECT_AND_CLOSE),
    _close_transport(control == TRANSPORT_CLOSE || control == TRANSPORT_CONNECT_AND_CLOSE),
    _tls_initialized(false),
    _handshake_completed(false),
    _cacert_allocated(false),
    _clicert_allocated(false),
    _ssl_conf_allocated(false)
{
#if defined(MBEDTLS_PLATFORM_C)
    int ret = mbedtls_platform_setup(NULL);
    if (ret != 0) {
        print_mbedtls_error("mbedtls_platform_setup()", ret);
    }
#endif /* MBEDTLS_PLATFORM_C */
    mbedtls_entropy_init(&_entropy);
    mbedtls_ctr_drbg_init(&_ctr_drbg);
    mbedtls_ssl_init(&_ssl);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_pk_init(&_pkctx);
#endif

    if (hostname) {
        set_hostname(hostname);
    }
}

TLSSocketWrapper::~TLSSocketWrapper()
{
    if (_transport) {
        close();
    }
    mbedtls_entropy_free(&_entropy);
    mbedtls_ctr_drbg_free(&_ctr_drbg);
    mbedtls_ssl_free(&_ssl);
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_pk_free(&_pkctx);
    set_own_cert(NULL);
    set_ca_chain(NULL);
#endif
    set_ssl_config(NULL);
#if defined(MBEDTLS_PLATFORM_C)
    mbedtls_platform_teardown(NULL);
#endif /* MBEDTLS_PLATFORM_C */
}

void TLSSocketWrapper::set_hostname(const char *hostname)
{
#ifdef MBEDTLS_X509_CRT_PARSE_C
    mbedtls_ssl_set_hostname(&_ssl, hostname);
#endif
}

nsapi_error_t TLSSocketWrapper::set_root_ca_cert(const void *root_ca, size_t len)
{
#if !defined(MBEDTLS_X509_CRT_PARSE_C)
    return NSAPI_ERROR_UNSUPPORTED;
#else
    mbedtls_x509_crt *crt;

    crt = new (std::nothrow) mbedtls_x509_crt;
    if (!crt) {
        return NSAPI_ERROR_NO_MEMORY;
    }

    mbedtls_x509_crt_init(crt);

    /* Parse CA certification */
    int ret;
    if ((ret = mbedtls_x509_crt_parse(crt, static_cast<const unsigned char *>(root_ca),
                                      len)) != 0) {
        print_mbedtls_error("mbedtls_x509_crt_parse", ret);
        mbedtls_x509_crt_free(crt);
        delete crt;
        return NSAPI_ERROR_PARAMETER;
    }
    set_ca_chain(crt);
    _cacert_allocated = true;
    return NSAPI_ERROR_OK;
#endif
}

nsapi_error_t TLSSocketWrapper::set_root_ca_cert(const char *root_ca_pem)
{
    return set_root_ca_cert(root_ca_pem, strlen(root_ca_pem) + 1);
}

nsapi_error_t TLSSocketWrapper::set_client_cert_key(const char *client_cert_pem, const char *client_private_key_pem)
{
    return set_client_cert_key(client_cert_pem, strlen(client_cert_pem) + 1, client_private_key_pem, strlen(client_private_key_pem) + 1);
}

nsapi_error_t TLSSocketWrapper::set_client_cert_key(const void *client_cert, size_t client_cert_len,
                                                    const void *client_private_key_pem, size_t client_private_key_len)
{
#if !defined(MBEDTLS_X509_CRT_PARSE_C) || !defined(MBEDTLS_PK_C)
    return NSAPI_ERROR_UNSUPPORTED;
#else

    int ret;
    mbedtls_x509_crt *crt = new mbedtls_x509_crt;
    mbedtls_x509_crt_init(crt);
    if ((ret = mbedtls_x509_crt_parse(crt, static_cast<const unsigned char *>(client_cert),
                                      client_cert_len)) != 0) {
        print_mbedtls_error("mbedtls_x509_crt_parse", ret);
        mbedtls_x509_crt_free(crt);
        delete crt;
        return NSAPI_ERROR_PARAMETER;
    }
    mbedtls_pk_init(&_pkctx);
    if ((ret = mbedtls_pk_parse_key(&_pkctx, static_cast<const unsigned char *>(client_private_key_pem),
                                    client_private_key_len, NULL, 0)) != 0) {
        print_mbedtls_error("mbedtls_pk_parse_key", ret);
        mbedtls_x509_crt_free(crt);
        delete crt;
        return NSAPI_ERROR_PARAMETER;
    }
    set_own_cert(crt);
    _clicert_allocated = true;

    return NSAPI_ERROR_OK;
#endif /* MBEDTLS_X509_CRT_PARSE_C */
}


nsapi_error_t TLSSocketWrapper::start_handshake(bool first_call)
{
    const char DRBG_PERS[] = "mbed TLS client";
    int ret;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (_tls_initialized) {
        return continue_handshake();
    }

#ifdef MBEDTLS_X509_CRT_PARSE_C
    tr_info("Starting TLS handshake with %s", _ssl.hostname);
#else
    tr_info("Starting TLS handshake");
#endif
    /*
     * Initialize TLS-related stuf.
     */
    if ((ret = mbedtls_ctr_drbg_seed(&_ctr_drbg, mbedtls_entropy_func, &_entropy,
                                     (const unsigned char *) DRBG_PERS,
                                     sizeof(DRBG_PERS))) != 0) {
        print_mbedtls_error("mbedtls_crt_drbg_init", ret);
        return NSAPI_ERROR_AUTH_FAILURE;
    }

    mbedtls_ssl_conf_rng(get_ssl_config(), mbedtls_ctr_drbg_random, &_ctr_drbg);


#if MBED_CONF_TLS_SOCKET_DEBUG_LEVEL > 0
    mbedtls_ssl_conf_verify(get_ssl_config(), my_verify, NULL);
    mbedtls_ssl_conf_dbg(get_ssl_config(), my_debug, NULL);
    mbedtls_debug_set_threshold(MBED_CONF_TLS_SOCKET_DEBUG_LEVEL);
#endif

    tr_debug("mbedtls_ssl_setup()");
    if ((ret = mbedtls_ssl_setup(&_ssl, get_ssl_config())) != 0) {
        print_mbedtls_error("mbedtls_ssl_setup", ret);
        return NSAPI_ERROR_AUTH_FAILURE;
    }

    _transport->set_blocking(false);
    _transport->sigio(mbed::callback(this, &TLSSocketWrapper::event));
    mbedtls_ssl_set_bio(&_ssl, this, ssl_send, ssl_recv, NULL);

    _tls_initialized = true;

    ret = continue_handshake();
    if (first_call) {
        if (ret == NSAPI_ERROR_ALREADY) {
            ret = NSAPI_ERROR_IN_PROGRESS; // If first call should return IN_PROGRESS
        }
        if (ret == NSAPI_ERROR_IS_CONNECTED) {
            ret = NSAPI_ERROR_OK;   // If we happened to complete the request on the first call, return OK.
        }
    }
    return ret;
}

nsapi_error_t TLSSocketWrapper::continue_handshake()
{
    int ret;

    if (_handshake_completed) {
        return NSAPI_ERROR_IS_CONNECTED;
    }

    if (!_tls_initialized) {
        return NSAPI_ERROR_NO_CONNECTION;
    }

    while (true) {
        ret = mbedtls_ssl_handshake(&_ssl);
        if (_timeout && (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                break;
            }
        } else {
            break;
        }
    }

    if (ret < 0) {
        print_mbedtls_error("mbedtls_ssl_handshake", ret);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return NSAPI_ERROR_ALREADY;
        } else {
            return NSAPI_ERROR_AUTH_FAILURE;
        }
    }

#ifdef MBEDTLS_X509_CRT_PARSE_C
    /* It also means the handshake is done, time to print info */
    tr_info("TLS connection to %s established", _ssl.hostname);
#else
    tr_info("TLS connection established");
#endif

#if defined(MBEDTLS_X509_CRT_PARSE_C) && defined(FEA_TRACE_SUPPORT)
    /* Prints the server certificate and verify it. */
    const size_t buf_size = 1024;
    char *buf = new char[buf_size];
    mbedtls_x509_crt_info(buf, buf_size, "\r    ",
                          mbedtls_ssl_get_peer_cert(&_ssl));
    tr_debug("Server certificate:\r\n%s\r\n", buf);

    uint32_t flags = mbedtls_ssl_get_verify_result(&_ssl);
    if (flags != 0) {
        /* Verification failed. */
        mbedtls_x509_crt_verify_info(buf, buf_size, "\r  ! ", flags);
        tr_error("Certificate verification failed:\r\n%s", buf);
    } else {
        /* Verification succeeded. */
        tr_info("Certificate verification passed");
    }
    delete[] buf;
#endif

    _handshake_completed = true;
    return NSAPI_ERROR_IS_CONNECTED;
}


nsapi_error_t TLSSocketWrapper::send(const void *data, nsapi_size_t size)
{
    int ret;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    tr_debug("send %d", size);
    while (true) {
        if (!_handshake_completed) {
            ret = continue_handshake();
            if (ret != NSAPI_ERROR_IS_CONNECTED) {
                if (ret == NSAPI_ERROR_ALREADY) {
                    ret = NSAPI_ERROR_WOULD_BLOCK;
                }
                return ret;
            }
        }

        ret = mbedtls_ssl_write(&_ssl, (const unsigned char *) data, size);

        if (_timeout == 0) {
            break;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                // Timeout break
                break;
            }
        } else {
            break;
        }
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_WANT_READ) {
        // translate to socket error
        return NSAPI_ERROR_WOULD_BLOCK;
    }

    if (ret < 0) {
        print_mbedtls_error("mbedtls_ssl_write", ret);
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return ret; // Assume "non negative errorcode" to be propagated from Socket layer
}

nsapi_size_or_error_t TLSSocketWrapper::sendto(const SocketAddress &, const void *data, nsapi_size_t size)
{
    // Ignore the SocketAddress
    return send(data, size);
}

nsapi_size_or_error_t TLSSocketWrapper::recv(void *data, nsapi_size_t size)
{
    int ret;

    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    while (true) {
        if (!_handshake_completed) {
            ret = continue_handshake();
            if (ret != NSAPI_ERROR_IS_CONNECTED) {
                if (ret == NSAPI_ERROR_ALREADY) {
                    ret = NSAPI_ERROR_WOULD_BLOCK;
                }
                return ret;
            }
        }

        ret = mbedtls_ssl_read(&_ssl, (unsigned char *) data, size);

        if (_timeout == 0) {
            break;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
            uint32_t flag;
            flag = _event_flag.wait_any(1, _timeout);
            if (flag & osFlagsError) {
                // Timeout break
                break;
            }
        } else {
            break;
        }
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_WANT_READ) {
        // translate to socket error
        return NSAPI_ERROR_WOULD_BLOCK;
    } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        /* MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY is not considered as error.
         * Just ignore here. Once connection is closed, mbedtls_ssl_read()
         * will return 0.
         */
        return 0;
    } else if (ret < 0) {
        print_mbedtls_error("mbedtls_ssl_read", ret);
        // There is no mapping of TLS error codes to Socket API so return most generic error to application
        return NSAPI_ERROR_DEVICE_ERROR;
    }
    return ret;
}

nsapi_size_or_error_t TLSSocketWrapper::recvfrom(SocketAddress *address, void *data, nsapi_size_t size)
{
    if (address) {
        getpeername(address);
    }
    return recv(data, size);
}

void TLSSocketWrapper::print_mbedtls_error(MBED_UNUSED const char *name, MBED_UNUSED int err)
{
// Avoid pulling in mbedtls_strerror when trace is not enabled
#if defined FEA_TRACE_SUPPORT && defined MBEDTLS_ERROR_C
    char *buf = new char[128];
    mbedtls_strerror(err, buf, 128);
    tr_err("%s() failed: -0x%04x (%d): %s", name, -err, err, buf);
    delete[] buf;
#else
    tr_err("%s() failed: -0x%04x (%d)", name, -err, err);
#endif
}


#if MBED_CONF_TLS_SOCKET_DEBUG_LEVEL > 0

void TLSSocketWrapper::my_debug(void *ctx, int level, const char *file, int line,
                                const char *str)
{
    const char *p, *basename;
    (void) ctx;

    /* Extract basename from file */
    for (p = basename = file; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            basename = p + 1;
        }
    }

    tr_debug("%s:%04d: |%d| %s", basename, line, level, str);
}


int TLSSocketWrapper::my_verify(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    const uint32_t buf_size = 1024;
    char *buf = new char[buf_size];
    (void) data;

    tr_debug("\nVerifying certificate at depth %d:\n", depth);
    mbedtls_x509_crt_info(buf, buf_size - 1, "  ", crt);
    tr_debug("%s", buf);

    if (*flags == 0) {
        tr_info("No verification issue for this certificate\n");
    } else {
        mbedtls_x509_crt_verify_info(buf, buf_size, "  ! ", *flags);
        tr_info("%s\n", buf);
    }

    delete[] buf;

    return 0;
}

#endif /* MBED_CONF_TLS_SOCKET_DEBUG_LEVEL > 0 */


int TLSSocketWrapper::ssl_recv(void *ctx, unsigned char *buf, size_t len)
{
    int recv;

    TLSSocketWrapper *my = static_cast<TLSSocketWrapper *>(ctx);

    if (!my->_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    recv = my->_transport->recv(buf, len);

    if (NSAPI_ERROR_WOULD_BLOCK == recv) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    } else if (recv < 0) {
        tr_error("Socket recv error %d", recv);
    }
    // Propagate also Socket errors to SSL, it allows negative error codes to be returned here.
    return recv;
}

int TLSSocketWrapper::ssl_send(void *ctx, const unsigned char *buf, size_t len)
{
    int size = -1;
    TLSSocketWrapper *my = static_cast<TLSSocketWrapper *>(ctx);

    if (!my->_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    size = my->_transport->send(buf, len);

    if (NSAPI_ERROR_WOULD_BLOCK == size) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    } else if (size < 0) {
        tr_error("Socket send error %d", size);
    }
    // Propagate also Socket errors to SSL, it allows negative error codes to be returned here.
    return size;
}

#if defined(MBEDTLS_X509_CRT_PARSE_C)

mbedtls_x509_crt *TLSSocketWrapper::get_own_cert()
{
    return _clicert;
}

int TLSSocketWrapper::set_own_cert(mbedtls_x509_crt *crt)
{
    int ret = 0;
    if (_clicert && _clicert_allocated) {
        mbedtls_x509_crt_free(_clicert);
        delete _clicert;
        _clicert_allocated = false;
    }
    _clicert = crt;
    if (crt) {
        if ((ret = mbedtls_ssl_conf_own_cert(get_ssl_config(), _clicert, &_pkctx)) != 0) {
            print_mbedtls_error("mbedtls_ssl_conf_own_cert", ret);
        }
    }
    return ret;
}

mbedtls_x509_crt *TLSSocketWrapper::get_ca_chain()
{
    return _cacert;
}

void TLSSocketWrapper::set_ca_chain(mbedtls_x509_crt *crt)
{
    if (_cacert && _cacert_allocated) {
        mbedtls_x509_crt_free(_cacert);
        delete _cacert;
        _cacert_allocated = false;
    }
    _cacert = crt;
    tr_debug("mbedtls_ssl_conf_ca_chain()");
    mbedtls_ssl_conf_ca_chain(get_ssl_config(), _cacert, NULL);
}

#endif /* MBEDTLS_X509_CRT_PARSE_C */

mbedtls_ssl_config *TLSSocketWrapper::get_ssl_config()
{
    if (!_ssl_conf) {
        int ret;
        _ssl_conf = new mbedtls_ssl_config;
        mbedtls_ssl_config_init(_ssl_conf);
        _ssl_conf_allocated = true;

        if ((ret = mbedtls_ssl_config_defaults(_ssl_conf,
                                               MBEDTLS_SSL_IS_CLIENT,
                                               MBEDTLS_SSL_TRANSPORT_STREAM,
                                               MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
            print_mbedtls_error("mbedtls_ssl_config_defaults", ret);
            set_ssl_config(NULL);
            MBED_ERROR(MBED_MAKE_ERROR(MBED_MODULE_NETWORK_STACK, MBED_ERROR_CODE_OUT_OF_MEMORY), "mbedtls_ssl_config_defaults() failed");
            return NULL;
        }
        /* It is possible to disable authentication by passing
         * MBEDTLS_SSL_VERIFY_NONE in the call to mbedtls_ssl_conf_authmode()
         */
        mbedtls_ssl_conf_authmode(get_ssl_config(), MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    return _ssl_conf;
}

void TLSSocketWrapper::set_ssl_config(mbedtls_ssl_config *conf)
{
    if (_ssl_conf && _ssl_conf_allocated) {
        mbedtls_ssl_config_free(_ssl_conf);
        delete _ssl_conf;
        _ssl_conf_allocated = false;
    }
    _ssl_conf = conf;
}

mbedtls_ssl_context *TLSSocketWrapper::get_ssl_context()
{
    return &_ssl;
}

nsapi_error_t TLSSocketWrapper::close()
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    tr_info("Closing TLS");

    int ret = 0;
    if (_handshake_completed) {
        _transport->set_blocking(true);
        ret = mbedtls_ssl_close_notify(&_ssl);
        if (ret) {
            print_mbedtls_error("mbedtls_ssl_close_notify", ret);
        }
        _handshake_completed = false;
    }

    if (_close_transport) {
        int ret2 = _transport->close();
        if (!ret) {
            ret = ret2;
        }
    }

    _transport = NULL;

    return ret;
}

nsapi_error_t TLSSocketWrapper::connect(const SocketAddress &address)
{
    nsapi_error_t ret = NSAPI_ERROR_OK;
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }

    if (!is_handshake_started() && _connect_transport) {
        ret = _transport->connect(address);
        if (ret && ret != NSAPI_ERROR_IS_CONNECTED) {
            return ret;
        }
    }
    return start_handshake(ret == NSAPI_ERROR_OK);
}

nsapi_error_t TLSSocketWrapper::bind(const SocketAddress &address)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->bind(address);
}

void TLSSocketWrapper::set_blocking(bool blocking)
{
    set_timeout(blocking ? -1 : 0);
}

void TLSSocketWrapper::set_timeout(int timeout)
{
    _timeout = timeout;
    if (!is_handshake_started() && timeout != -1 && _connect_transport) {
        // If we have not yet connected the transport, we need to modify its blocking mode as well.
        // After connection is initiated, it is already set to non blocking mode
        _transport->set_timeout(timeout);
    }
}

void TLSSocketWrapper::sigio(mbed::Callback<void()> func)
{
    if (!_transport) {
        return;
    }
    _sigio = func;
    _transport->sigio(mbed::callback(this, &TLSSocketWrapper::event));
}

nsapi_error_t TLSSocketWrapper::setsockopt(int level, int optname, const void *optval, unsigned optlen)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->setsockopt(level, optname, optval, optlen);
}

nsapi_error_t TLSSocketWrapper::getsockopt(int level, int optname, void *optval, unsigned *optlen)
{
    if (!_transport) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    return _transport->getsockopt(level, optname, optval, optlen);
}

Socket *TLSSocketWrapper::accept(nsapi_error_t *err)
{
    if (err) {
        *err = NSAPI_ERROR_UNSUPPORTED;
    }
    return NULL;
}

nsapi_error_t TLSSocketWrapper::listen(int)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

void TLSSocketWrapper::event()
{
    _event_flag.set(1);
    if (_sigio) {
        _sigio();
    }
}

bool TLSSocketWrapper::is_handshake_started() const
{
    return _tls_initialized;
}


nsapi_error_t TLSSocketWrapper::getpeername(SocketAddress *address)
{
    if (!_handshake_completed) {
        return NSAPI_ERROR_NO_CONNECTION;
    }
    return _transport->getpeername(address);
}

#endif