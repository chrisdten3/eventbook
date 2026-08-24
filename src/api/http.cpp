#include "eventbook/api/http.hpp"

namespace eventbook {

std::string_view to_string(HttpError error) {
    switch (error) {
        case HttpError::InvalidRequest:
            return "malformed request";
        case HttpError::ResolveFailed:
            return "DNS resolution failed";
        case HttpError::ConnectFailed:
            return "TCP connection failed";
        case HttpError::TlsContextFailed:
            return "TLS context could not be prepared";
        case HttpError::TlsHandshakeFailed:
            return "TLS handshake or certificate verification failed";
        case HttpError::WriteFailed:
            return "request write failed";
        case HttpError::ReadFailed:
            return "response read failed";
        case HttpError::Timeout:
            return "request timed out";
    }
    return "unknown http error";
}

}  // namespace eventbook
