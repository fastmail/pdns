/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2002-2016  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation

    Additionally, the license of this program contains a special
    exception which allows to distribute the program in binary form when
    it is linked against OpenSSL.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "utility.hh"
#include "webserver.hh"
#include "misc.hh"
#include <vector>
#include "logger.hh"
#include <stdio.h>
#include "dns.hh"
#include "base64.hh"
#include "json.hh"
#include "arguments.hh"
#include <yahttp/router.hpp>

struct connectionThreadData {
  WebServer* webServer{nullptr};
  Socket* client{nullptr};
};

json11::Json HttpRequest::json()
{
  string err;
  if(this->body.empty()) {
    L<<Logger::Debug<<"HTTP: JSON document expected in request body, but body was empty" << endl;
    throw HttpBadRequestException();
  }
  json11::Json doc = json11::Json::parse(this->body, err);
  if (doc.is_null()) {
    L<<Logger::Debug<<"HTTP: parsing of JSON document failed:" << err << endl;
    throw HttpBadRequestException();
  }
  return doc;
}

bool HttpRequest::compareAuthorization(const string &expected_password)
{
  // validate password
  YaHTTP::strstr_map_t::iterator header = headers.find("authorization");
  bool auth_ok = false;
  if (header != headers.end() && toLower(header->second).find("basic ") == 0) {
    string cookie = header->second.substr(6);

    string plain;
    B64Decode(cookie, plain);

    vector<string> cparts;
    stringtok(cparts, plain, ":");

    // this gets rid of terminating zeros
    auth_ok = (cparts.size()==2 && (0==strcmp(cparts[1].c_str(), expected_password.c_str())));
  }
  return auth_ok;
}

bool HttpRequest::compareHeader(const string &header_name, const string &expected_value)
{
  YaHTTP::strstr_map_t::iterator header = headers.find(header_name);
  if (header == headers.end())
    return false;

  // this gets rid of terminating zeros
  return (0==strcmp(header->second.c_str(), expected_value.c_str()));
}


void HttpResponse::setBody(const json11::Json& document)
{
  document.dump(this->body);
}

void HttpResponse::setErrorResult(const std::string& message, const int status)
{
  setBody(json11::Json::object { { "error", message } });
  this->status = status;
}

void HttpResponse::setSuccessResult(const std::string& message, const int status)
{
  setBody(json11::Json::object { { "result", message } });
  this->status = status;
}

static void bareHandlerWrapper(WebServer::HandlerFunction handler, YaHTTP::Request* req, YaHTTP::Response* resp)
{
  // wrapper to convert from YaHTTP::* to our subclasses
  handler(static_cast<HttpRequest*>(req), static_cast<HttpResponse*>(resp));
}

void WebServer::registerBareHandler(const string& url, HandlerFunction handler)
{
  YaHTTP::THandlerFunction f = std::bind(&bareHandlerWrapper, handler, std::placeholders::_1, std::placeholders::_2);
  YaHTTP::Router::Any(url, f);
}

static bool optionsHandler(HttpRequest* req, HttpResponse* resp) {
  if (req->method == "OPTIONS") {
    resp->headers["access-control-allow-origin"] = "*";
    resp->headers["access-control-allow-headers"] = "Content-Type, X-API-Key";
    resp->headers["access-control-allow-methods"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
    resp->headers["access-control-max-age"] = "3600";
    resp->status = 200;
    resp->headers["content-type"]= "text/plain";
    resp->body = "";
    return true;
  }
  return false;
}

static void apiWrapper(WebServer::HandlerFunction handler, HttpRequest* req, HttpResponse* resp) {
  const string& api_key = arg()["api-key"];

  if (optionsHandler(req, resp)) return;

  resp->headers["access-control-allow-origin"] = "*";

  if (api_key.empty()) {
    L<<Logger::Error<<"HTTP API Request \"" << req->url.path << "\": Authentication failed, API Key missing in config" << endl;
    throw HttpUnauthorizedException("X-API-Key");
  }
  bool auth_ok = req->compareHeader("x-api-key", api_key) || req->getvars["api-key"]==api_key;
  
  if (!auth_ok) {
    L<<Logger::Error<<"HTTP Request \"" << req->url.path << "\": Authentication by API Key failed" << endl;
    throw HttpUnauthorizedException("X-API-Key");
  }

  resp->headers["Content-Type"] = "application/json";

  // security headers
  resp->headers["X-Content-Type-Options"] = "nosniff";
  resp->headers["X-Frame-Options"] = "deny";
  resp->headers["X-Permitted-Cross-Domain-Policies"] = "none";
  resp->headers["X-XSS-Protection"] = "1; mode=block";
  resp->headers["Content-Security-Policy"] = "default-src 'self'; style-src 'self' 'unsafe-inline'";

  req->getvars.erase("_"); // jQuery cache buster

  try {
    resp->status = 200;
    handler(req, resp);
  } catch (ApiException &e) {
    resp->setErrorResult(e.what(), 422);
    return;
  } catch (JsonException &e) {
    resp->setErrorResult(e.what(), 422);
    return;
  }

  if (resp->status == 204) {
    // No Content -> no Content-Type.
    resp->headers.erase("Content-Type");
  }
}

void WebServer::registerApiHandler(const string& url, HandlerFunction handler) {
  HandlerFunction f = std::bind(&apiWrapper, handler, std::placeholders::_1, std::placeholders::_2);
  registerBareHandler(url, f);
}

static void webWrapper(WebServer::HandlerFunction handler, HttpRequest* req, HttpResponse* resp) {
  const string& web_password = arg()["webserver-password"];

  if (!web_password.empty()) {
    bool auth_ok = req->compareAuthorization(web_password);
    if (!auth_ok) {
      L<<Logger::Debug<<"HTTP Request \"" << req->url.path << "\": Web Authentication failed" << endl;
      throw HttpUnauthorizedException("Basic");
    }
  }

  handler(req, resp);
}

void WebServer::registerWebHandler(const string& url, HandlerFunction handler) {
  HandlerFunction f = std::bind(&webWrapper, handler, std::placeholders::_1, std::placeholders::_2);
  registerBareHandler(url, f);
}

static void *WebServerConnectionThreadStart(void *p) {
  connectionThreadData* data = static_cast<connectionThreadData*>(p);
  pthread_detach(pthread_self());
  data->webServer->serveConnection(data->client);

  delete data->client; // close socket
  delete data;

  return NULL;
}

void WebServer::handleRequest(HttpRequest& req, HttpResponse& resp)
{
  // set default headers
  resp.headers["Content-Type"] = "text/html; charset=utf-8";

  try {
    if (!req.complete) {
      L<<Logger::Debug<<"HTTP: Incomplete request" << endl;
      throw HttpBadRequestException();
    }

    L<<Logger::Debug<<"HTTP: Handling request \"" << req.url.path << "\"" << endl;

    YaHTTP::strstr_map_t::iterator header;

    if ((header = req.headers.find("accept")) != req.headers.end()) {
      // json wins over html
      if (header->second.find("application/json") != std::string::npos) {
        req.accept_json = true;
      } else if (header->second.find("text/html") != std::string::npos) {
        req.accept_html = true;
      }
    }

    YaHTTP::THandlerFunction handler;
    if (!YaHTTP::Router::Route(&req, handler)) {
      L<<Logger::Debug<<"HTTP: No route found for \"" << req.url.path << "\"" << endl;
      throw HttpNotFoundException();
    }

    try {
      handler(&req, &resp);
      L<<Logger::Debug<<"HTTP: Result for \"" << req.url.path << "\": " << resp.status << ", body length: " << resp.body.size() << endl;
    }
    catch(HttpException&) {
      throw;
    }
    catch(PDNSException &e) {
      L<<Logger::Error<<"HTTP ISE for \""<< req.url.path << "\": Exception: " << e.reason << endl;
      throw HttpInternalServerErrorException();
    }
    catch(std::exception &e) {
      L<<Logger::Error<<"HTTP ISE for \""<< req.url.path << "\": STL Exception: " << e.what() << endl;
      throw HttpInternalServerErrorException();
    }
    catch(...) {
      L<<Logger::Error<<"HTTP ISE for \""<< req.url.path << "\": Unknown Exception" << endl;
      throw HttpInternalServerErrorException();
    }
  }
  catch(HttpException &e) {
    resp = e.response();
    L<<Logger::Debug<<"HTTP: Error result for \"" << req.url.path << "\": " << resp.status << endl;
    string what = YaHTTP::Utility::status2text(resp.status);
    if(req.accept_html) {
      resp.headers["Content-Type"] = "text/html; charset=utf-8";
      resp.body = "<!html><title>" + what + "</title><h1>" + what + "</h1>";
    } else if (req.accept_json) {
      resp.headers["Content-Type"] = "application/json";
      resp.setErrorResult(what, resp.status);
    } else {
      resp.headers["Content-Type"] = "text/plain; charset=utf-8";
      resp.body = what;
    }
  }

  // always set these headers
  resp.headers["Server"] = "PowerDNS/" VERSION;
  resp.headers["Connection"] = "close";

  if (req.method == "HEAD") {
    resp.body = "";
  } else {
    resp.headers["Content-Length"] = std::to_string(resp.body.size());
  }
}

void WebServer::serveConnection(Socket *client)
try {
  HttpRequest req;
  YaHTTP::AsyncRequestLoader yarl;
  yarl.initialize(&req);
  int timeout = 5;
  client->setNonBlocking();

  try {
    while(!req.complete) {
      int bytes;
      char buf[1024];
      bytes = client->readWithTimeout(buf, sizeof(buf), timeout);
      if (bytes > 0) {
        string data = string(buf, bytes);
        req.complete = yarl.feed(data);
      } else {
        // read error OR EOF
        break;
      }
    }
    yarl.finalize();
  } catch (YaHTTP::ParseError &e) {
    // request stays incomplete
  }

  HttpResponse resp;
  WebServer::handleRequest(req, resp);
  ostringstream ss;
  resp.write(ss);
  string reply = ss.str();

  client->writenWithTimeout(reply.c_str(), reply.size(), timeout);
}
catch(PDNSException &e) {
  L<<Logger::Error<<"HTTP Exception: "<<e.reason<<endl;
}
catch(std::exception &e) {
  if(strstr(e.what(), "timeout")==0)
    L<<Logger::Error<<"HTTP STL Exception: "<<e.what()<<endl;
}
catch(...) {
  L<<Logger::Error<<"HTTP: Unknown exception"<<endl;
}

WebServer::WebServer(const string &listenaddress, int port) : d_server(NULL)
{
  d_listenaddress=listenaddress;
  d_port=port;
}

void WebServer::bind()
{
  try {
    d_server = createServer();
    L<<Logger::Warning<<"Listening for HTTP requests on "<<d_server->d_local.toStringWithPort()<<endl;
  }
  catch(NetworkError &e) {
    L<<Logger::Error<<"Listening on HTTP socket failed: "<<e.what()<<endl;
    d_server = NULL;
  }
}

void WebServer::go()
{
  if(!d_server)
    return;
  try {
    pthread_t tid;

    NetmaskGroup acl;
    acl.toMasks(::arg()["webserver-allow-from"]);

    while(true) {
      // data and data->client will be freed by thread
      connectionThreadData *data = new connectionThreadData;
      data->webServer = this;
      try {
        data->client = d_server->accept();
        if (data->client->acl(acl)) {
          pthread_create(&tid, 0, &WebServerConnectionThreadStart, (void *)data);
        } else {
          ComboAddress remote;
          if (data->client->getRemote(remote))
            L<<Logger::Error<<"Webserver closing socket: remote ("<< remote.toString() <<") does not match 'webserver-allow-from'"<<endl;
          delete data->client; // close socket
          delete data;
        }
      }
      catch(PDNSException &e) {
        L<<Logger::Error<<"PDNSException while accepting a connection in main webserver thread: "<<e.reason<<endl;
        delete data->client;
        delete data;
      }
      catch(std::exception &e) {
        L<<Logger::Error<<"STL Exception while accepting a connection in main webserver thread: "<<e.what()<<endl;
        delete data->client;
        delete data;
      }
      catch(...) {
        L<<Logger::Error<<"Unknown exception while accepting a connection in main webserver thread"<<endl;
        delete data->client;
        delete data;
      }
    }
  }
  catch(PDNSException &e) {
    L<<Logger::Error<<"PDNSException in main webserver thread: "<<e.reason<<endl;
  }
  catch(std::exception &e) {
    L<<Logger::Error<<"STL Exception in main webserver thread: "<<e.what()<<endl;
  }
  catch(...) {
    L<<Logger::Error<<"Unknown exception in main webserver thread"<<endl;
  }
  exit(1);
}
