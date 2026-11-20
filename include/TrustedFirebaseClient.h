#pragma once

#include <WiFiClientSecure.h>
#include "sdkconfig.h"
#include "ssl_client.h"

class TrustedFirebaseClient : public WiFiClientSecure {
public:
    bool useSystemCertificateBundle() {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        if (!sslclient) return false;
        attach_ssl_certificate_bundle(sslclient.get(), true);
        _use_ca_bundle = true;
        _use_insecure = false;
        return true;
#else
        return false;
#endif
    }
};
