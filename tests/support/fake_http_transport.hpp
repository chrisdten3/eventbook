#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "eventbook/api/http.hpp"

namespace eventbook::testing {

/// An HttpTransport that answers from a script instead of a network.
///
/// This is the payoff for making HttpTransport an interface. Pagination, status
/// handling, and (in the next slice) eligibility rules can be tested against
/// recorded payloads: deterministic, offline, and unaffected by what happens to
/// be listed on Kalshi this afternoon.
///
/// It also records what it was asked for, which is how the read-only guarantee
/// gets asserted rather than assumed.
class FakeHttpTransport final : public HttpTransport {
public:
    using Reply = Result<HttpResponse, HttpError>;

    /// Replies are consumed in order, one per send().
    void enqueue(Reply reply) {
        replies_.push_back(std::move(reply));
    }

    void enqueue_ok(int status_code, std::string body) {
        enqueue(HttpResponse{status_code, std::move(body)});
    }

    [[nodiscard]] Result<HttpResponse, HttpError> send(const HttpRequest& request) override {
        requests_.push_back(request);
        if (next_ >= replies_.size()) {
            return Failure{HttpError::ConnectFailed};
        }
        return replies_[next_++];
    }

    [[nodiscard]] const std::vector<HttpRequest>& requests() const {
        return requests_;
    }

    [[nodiscard]] const HttpRequest& last_request() const {
        return requests_.back();
    }

private:
    std::vector<Reply> replies_;
    std::vector<HttpRequest> requests_;
    std::size_t next_{0};
};

}  // namespace eventbook::testing
