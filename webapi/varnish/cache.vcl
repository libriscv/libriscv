vcl 4.0;
import std;
import accept;
import bodyaccess;
import file;

backend default {
	.host = "localhost";
	.port = "1234";
}

acl authorized {
	"localhost";
	"127.0.0.1";
	"::1";
}

sub vcl_init {
	new format = accept.rule("text/plain");
	new fs = file.init("/path/to/varnish/www");
}

sub vcl_recv
{
	if (std.port(local.ip) == 80) {
		if (client.ip ~ authorized) {
			set req.http.x-hmethod = req.method;
			std.cache_req_body(15MB);
			return (hash);
		}
		return (synth(700, "https://my.web.site/"));
	}

	if (client.ip ~ authorized) {
		std.cache_req_body(40MB);
		return (hash);
	}
	if (req.url ~ "/exec\?*.*" && req.method == "POST") {
		# compiling and executing a binary
		set req.backend_hint = default;
		# accept VMOD
		set req.http.accept = format.filter(req.http.accept);
		# bodyaccess VMOD
		set req.http.x-hmethod = req.method;
		set req.http.x-cache = "nope";
		return (pass);
	}
	else if (req.url == "/") {
		# displaying the index page
		return (hash);
	}
	return (synth(401));
}

sub vcl_hash {
	bodyaccess.hash_req_body();
}

sub vcl_backend_fetch
{
	if (bereq.url == "/") {
		# Set the file system as the backend
		set bereq.backend = fs.backend();
		set bereq.url = "/index.html";
	} else {
		set bereq.method = bereq.http.x-hmethod;
	}
}

sub vcl_synth {
	if (resp.status == 700) {
		set resp.http.Location = resp.reason;
		set resp.status = 301;
		return (deliver);
	}
}
