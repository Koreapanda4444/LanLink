#include "windows_certificate.hpp"

#include <openssl/pem.h>

#include <windows.h>
#include <wincrypt.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace lanlink::transport {

class SchannelCertificate::Impl {
public:
    ~Impl() {
        if (certificate_) CertFreeCertificateContext(certificate_);
        if (store_) CertCloseStore(store_, 0);
    }

    void load(const std::filesystem::path& certificate_path) {
        const auto input = std::unique_ptr<BIO, decltype(&BIO_free)>(
            BIO_new_file(certificate_path.string().c_str(), "rb"), BIO_free);
        if (!input) throw std::runtime_error("cannot open relay PEM certificate");
        const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(
            PEM_read_bio_X509_AUX(input.get(), nullptr, nullptr, nullptr), X509_free);
        std::array<unsigned char, EVP_MAX_MD_SIZE> fingerprint{};
        unsigned int size = 0;
        if (!certificate || X509_digest(certificate.get(), EVP_sha1(), fingerprint.data(), &size) != 1) {
            throw std::runtime_error("cannot identify relay certificate for Schannel");
        }
        CRYPT_HASH_BLOB hash{size, fingerprint.data()};
        for (const auto location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
            auto store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                location | CERT_STORE_READONLY_FLAG, L"MY");
            if (!store) continue;
            auto context = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &hash, nullptr);
            DWORD property_size = 0;
            if (context && CertGetCertificateContextProperty(context, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &property_size)) {
                store_ = store;
                certificate_ = context;
                return;
            }
            if (context) CertFreeCertificateContext(context);
            CertCloseStore(store, 0);
        }
        throw std::runtime_error("Windows relay certificate and private key must be imported into the CurrentUser or LocalMachine My certificate store");
    }

    void load_pkcs12(std::span<const std::byte> bytes) {
        if(bytes.empty() || bytes.size()>1024*1024)throw std::invalid_argument("invalid in-memory peer certificate");
        CRYPT_DATA_BLOB blob{static_cast<DWORD>(bytes.size()),reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data()))};
        store_=PFXImportCertStore(&blob,L"",PKCS12_NO_PERSIST_KEY|PKCS12_ALWAYS_CNG_KSP);
        if(!store_)throw std::runtime_error("cannot import in-memory peer certificate");
        certificate_=CertEnumCertificatesInStore(store_,nullptr);
        if(!certificate_)throw std::runtime_error("peer certificate store is empty");
    }

    HCERTSTORE store_ = nullptr;
    PCCERT_CONTEXT certificate_ = nullptr;
};

SchannelCertificate::SchannelCertificate(const std::filesystem::path& certificate,
                                       const std::filesystem::path& private_key)
    : impl_(std::make_unique<Impl>()) {
    if (private_key.empty()) throw std::invalid_argument("relay private key path is required");
    impl_->load(certificate);
}
SchannelCertificate::SchannelCertificate(std::span<const std::byte> pkcs12):impl_(std::make_unique<Impl>()) {
    impl_->load_pkcs12(pkcs12);
}
SchannelCertificate::~SchannelCertificate() = default;
void* SchannelCertificate::context() const noexcept { return const_cast<CERT_CONTEXT*>(impl_->certificate_); }

}
