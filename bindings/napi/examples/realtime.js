"use strict";
const { startChat } = require("./chat/server");
module.exports = { startChat };

if (require.main === module) {
  const port = process.env.PORT === undefined ? 8080 : Number(process.env.PORT);
  if (!Number.isInteger(port) || port < 0 || port > 65535) {
    console.error("PORT must be an integer from 0 to 65535.");
    process.exitCode = 1;
  } else {
    startChat({ port, host: process.env.HOST || "127.0.0.1" })
      .then((chat) => {
        console.log("Tinyactors chat: " + chat.url);
        console.log("Open this URL in multiple tabs. Ctrl-C stops the server.");
        let closing = false;
        function shutdown() {
          if (closing) return;
          closing = true;
          chat.close().catch((error) => {
            console.error(error);
            process.exitCode = 1;
          });
        }
        process.once("SIGINT", shutdown);
        process.once("SIGTERM", shutdown);
      })
      .catch((error) => {
        console.error(error);
        process.exitCode = 1;
      });
  }
}
