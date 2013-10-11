ssl_options1 = {sslMode : "sslOnly",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
ssl_options2 = ssl_options1;
load("jstests/replsets/replset1.js");

// Test mixed sslMode acceptSSL/sendAcceptSSL
ssl_options1 = {sslMode : "acceptSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
ssl_options2 = {sslMode : "sendAcceptSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
load("jstests/replsets/replset1.js");

// Test mixed sslMode sendAcceptSSL/sslOnly
ssl_options1 = {sslMode : "sendAcceptSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
ssl_options2 = {sslMode : "sslOnly",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
load("jstests/replsets/replset1.js");
