#include "support/http_client.hpp"

#include <curl/curl.h>

#include <mutex>

namespace sb::test {

namespace {

std::once_flag g_curl_once;

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

std::size_t read_header(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  std::string line(ptr, size * nmemb);
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return size * nmemb;
  }
  std::string key = line.substr(0, colon);
  std::string value = line.substr(colon + 1);
  for (char& c : key) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
    value.pop_back();
  }
  std::size_t begin = 0;
  while (begin < value.size() && value[begin] == ' ') {
    ++begin;
  }
  (*headers)[key] = value.substr(begin);
  return size * nmemb;
}

} // namespace

HttpResponse http_request(int port, const std::string& method, const std::string& path,
                          const std::string& body, const std::vector<std::string>& headers) {
  HttpResponse out;
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return out;
  }

  const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.headers);

  struct curl_slist* list = nullptr;
  for (const auto& header : headers) {
    list = curl_slist_append(list, header.c_str());
  }
  if (list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  }
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
  }
  if (list != nullptr) {
    curl_slist_free_all(list);
  }
  curl_easy_cleanup(curl);
  return out;
}

} // namespace sb::test