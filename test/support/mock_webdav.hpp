#pragma once

#include "webdav.hpp"

#include <gmock/gmock.h>

#include <string>
#include <vector>

namespace sb::test {

class MockWebDav : public webdav::IWebDavClient {
public:
  MOCK_METHOD(webdav::Response, propfind,
              (const webdav::Url&, const std::string&, int, const webdav::ClientOptions&),
              (override));

  MOCK_METHOD(webdav::Stream, open_stream,
              (const webdav::Url&, const std::string&, const std::vector<std::string>&,
               const webdav::ClientOptions&),
              (override));
};

} // namespace sb::test