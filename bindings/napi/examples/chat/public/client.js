(function () {
  "use strict";

  class ChatComposer extends HTMLElement {
    connectedCallback() {
      this.setAttribute("role", "group");
      this.setAttribute("aria-label", "Message composer");
    }
  }

  if (!customElements.get("chat-composer"))
    customElements.define("chat-composer", ChatComposer);

  class ChatReadReceipts extends HTMLElement {
    set receipts(value) {
      this._receipts = Array.isArray(value) ? value : [];
      this.render();
    }
    connectedCallback() { this.render(); }
    render() {
      this.replaceChildren();
      var button = document.createElement("button");
      button.type = "button";
      button.className = "receipt-button";
      button.setAttribute("aria-label", "Who has seen this message?");
      button.textContent = this._receipts?.length ? "✓✓ " + this._receipts.length : "✓";
      button.addEventListener("click", () => {
        this.dispatchEvent(new CustomEvent("receipts-open", { bubbles: true, detail: this._receipts }));
      });
      this.appendChild(button);
    }
  }
  class ChatMessage extends HTMLElement {
    connectedCallback() {
      var id = Number(this.dataset.messageId);
      if (window.IntersectionObserver) {
        var observer = new IntersectionObserver((entries) => {
          if (entries.some((entry) => entry.isIntersecting)) {
            this.dispatchEvent(new CustomEvent("message-read", { bubbles: true, detail: id }));
            observer.disconnect();
          }
        }, { threshold: 0.5 });
        observer.observe(this);
      } else this.dispatchEvent(new CustomEvent("message-read", { bubbles: true, detail: id }));
    }
  }
  class ChatReceiptPopover extends HTMLElement {
    set receipts(value) { this._receipts = Array.isArray(value) ? value : []; this.render(); }
    connectedCallback() { this.render(); }
    render() {
      this.className = "receipt-popover";
      this.replaceChildren();
      this._receipts.forEach(function (receipt) {
        var row = document.createElement("div");
        row.textContent = String(receipt.user || "Unknown") + " · " + new Date(receipt.seenAt).toLocaleTimeString();
        this.appendChild(row);
      }, this);
    }
  }
  if (!customElements.get("chat-read-receipts")) customElements.define("chat-read-receipts", ChatReadReceipts);
  if (!customElements.get("chat-message")) customElements.define("chat-message", ChatMessage);
  if (!customElements.get("chat-receipt-popover")) customElements.define("chat-receipt-popover", ChatReceiptPopover);

  var state = {
    socket: null,
    name: "",
    rooms: [],
    room: { room: null, members: [], messages: [] },
    ready: false,
    wantsConnection: false,
    retry: 0,
    retryTimer: null,
    freshNext: false,
    replaced: false,
  };

  function byId(id) {
    return document.getElementById(id);
  }

  function readStorage(key) {
    try {
      return sessionStorage.getItem(key) || "";
    } catch (error) {
      return "";
    }
  }

  function writeStorage(key, value) {
    try {
      sessionStorage.setItem(key, value);
    } catch (error) {
      /* Storage can be disabled. */
    }
  }

  function removeStorage(key) {
    try {
      sessionStorage.removeItem(key);
    } catch (error) {
      /* Storage can be disabled. */
    }
  }

  function setStatus(kind, text) {
    var item = byId("connection-status");
    item.className = "status status-" + kind;
    item.textContent = text;
  }

  function setError(text) {
    byId("error-message").textContent = text || "";
  }

  function validName(name) {
    return name.trim().length > 0 && name.trim().length <= 32;
  }

  function validRoom(name) {
    return /^[a-z0-9_-]{1,32}$/.test(name);
  }

  function socketUrl() {
    var scheme = location.protocol === "https:" ? "wss:" : "ws:";
    return scheme + "//" + location.host + "/chat";
  }

  function sendCommand(command) {
    if (
      !state.socket ||
      state.socket.readyState !== WebSocket.OPEN ||
      !state.ready
    ) {
      setError("You are offline. Wait for the connection before sending.");
      return false;
    }
    try {
      state.socket.send(JSON.stringify(command));
      return true;
    } catch (error) {
      setError("The connection could not send that command.");
      return false;
    }
  }

  function connect(fresh) {
    if (!state.name || !state.wantsConnection) return;
    if (
      state.socket &&
      (state.socket.readyState === WebSocket.OPEN ||
        state.socket.readyState === WebSocket.CONNECTING)
    )
      return;
    clearRetry();
    if (fresh) removeStorage("tinyactors-chat-resume");
    state.replaced = false;
    state.ready = false;
    setStatus(
      state.retry ? "reconnecting" : "connecting",
      state.retry ? "Reconnecting…" : "Connecting…",
    );
    byId("connection-note").textContent = "Connecting as " + state.name + "…";
    var socket = new WebSocket(socketUrl());
    state.socket = socket;
    socket.addEventListener("open", function () {
      handleOpen(socket);
    });
    socket.addEventListener("message", function (event) {
      handleMessage(socket, event);
    });
    socket.addEventListener("close", function (event) {
      handleClose(socket, event);
    });
    socket.addEventListener("error", function () {
      handleError(socket);
    });
    updateControls();
  }

  function handleOpen(socket) {
    if (socket !== state.socket) return;
    var hello = { type: "hello", name: state.name };
    var resume = readStorage("tinyactors-chat-resume");
    if (resume) hello.resume = resume;
    try {
      socket.send(JSON.stringify(hello));
    } catch (error) {
      socket.close();
    }
  }

  function handleError(socket) {
    if (socket !== state.socket) return;
    setStatus("reconnecting", "Connection interrupted");
  }

  function handleClose(socket, event) {
    if (socket !== state.socket) return;
    state.socket = null;
    state.ready = false;
    updateControls();
    if (event && event.code === 4000) {
      state.wantsConnection = false;
      state.replaced = true;
      removeStorage("tinyactors-chat-resume");
      clearRetry();
      setStatus("offline", "Replaced by another tab");
      byId("connection-note").textContent =
        "This session was replaced. Start a fresh session to continue.";
      setError(
        "This connection was replaced by another session. Choose Start fresh session to continue.",
      );
      updateControls();
      return;
    }
    if (state.wantsConnection) scheduleReconnect();
    else setStatus("offline", "Offline");
  }

  function clearRetry() {
    if (state.retryTimer) window.clearTimeout(state.retryTimer);
    state.retryTimer = null;
  }

  function scheduleReconnect() {
    if (state.retryTimer || !state.wantsConnection) return;
    var delay = Math.min(10000, 400 * Math.pow(2, state.retry));
    state.retry += 1;
    setStatus(
      "reconnecting",
      "Reconnecting in " + Math.ceil(delay / 1000) + "s",
    );
    state.retryTimer = window.setTimeout(function () {
      state.retryTimer = null;
      connect(state.freshNext);
      state.freshNext = false;
    }, delay);
  }

  function handleMessage(socket, event) {
    if (socket !== state.socket || typeof event.data !== "string") return;
    var frame;
    try {
      frame = JSON.parse(event.data);
    } catch (error) {
      setError("The server sent an invalid frame.");
      return;
    }
    if (!frame || typeof frame.type !== "string") return;
    if (frame.type === "welcome") handleWelcome(frame);
    else if (frame.type === "rooms") handleRooms(frame);
    else if (frame.type === "room") handleRoom(frame);
    else if (frame.type === "receipt") handleReceipt(frame);
    else if (frame.type === "error") handleServerError(frame);
  }

  function handleWelcome(frame) {
    if (typeof frame.session === "string" && frame.session)
      writeStorage("tinyactors-chat-resume", frame.session);
    if (typeof frame.user === "string" && frame.user) {
      state.name = frame.user;
      byId("name-input").value = frame.user;
      writeStorage("tinyactors-chat-name", frame.user);
    }
    if (frame.resumed === false) {
      state.rooms = [];
      state.room = { room: null, members: [], messages: [] };
      renderRooms();
      renderRoom();
    }
    state.ready = true;
    state.retry = 0;
    state.freshNext = false;
    setStatus("online", "Online");
    byId("connection-note").textContent = frame.resumed
      ? "Session resumed."
      : "Connected as " + state.name + ".";
    setError("");
    updateControls();
    sendCommand({ type: "list" });
  }

  function handleRooms(frame) {
    state.rooms = Array.isArray(frame.rooms) ? frame.rooms : [];
    renderRooms();
  }

  function handleRoom(frame) {
    state.room = {
      room: typeof frame.room === "string" ? frame.room : null,
      members: Array.isArray(frame.members) ? frame.members : [],
      messages: Array.isArray(frame.messages) ? frame.messages : [],
    };
    renderRoom();
    updateControls();
  }

  function handleReceipt(frame) {
    if (frame.room !== state.room.room) return;
    var message = state.room.messages.find(function (item) { return item.id === frame.messageId; });
    if (!message) return;
    message.receipts = Array.isArray(frame.receipts) ? frame.receipts : [];
    renderMessages(state.room.messages);
  }

  function handleServerError(frame) {
    setError(
      typeof frame.message === "string"
        ? frame.message
        : "The server rejected that request.",
    );
    if (frame.code === "session_expired") {
      removeStorage("tinyactors-chat-resume");
      state.freshNext = true;
      if (state.socket) state.socket.close();
    }
  }

  function updateControls() {
    var active =
      state.ready && state.socket && state.socket.readyState === WebSocket.OPEN;
    var activeSocket =
      state.socket &&
      (state.socket.readyState === WebSocket.OPEN ||
        state.socket.readyState === WebSocket.CONNECTING);
    var inRoom = active && Boolean(state.room.room);
    byId("name-input").disabled = Boolean(activeSocket);
    byId("connect-button").disabled = Boolean(activeSocket);
    byId("toggle-connection").textContent = state.replaced
      ? "Start fresh session"
      : activeSocket
        ? "Disconnect"
        : "Reconnect";
    byId("room-input").disabled = !active;
    byId("create-button").disabled = !active;
    byId("refresh-rooms").disabled = !active;
    byId("leave-button").disabled = !inRoom;
    byId("message-input").disabled = !inRoom;
    byId("message-input").placeholder = inRoom
      ? "Write a message"
      : "Connect and join a room to write a message";
    byId("send-button").disabled = !inRoom;
    document.querySelectorAll(".room-button").forEach(function (button) {
      button.disabled = !active;
    });
  }

  function renderRooms() {
    var list = byId("room-list");
    list.replaceChildren();
    if (!state.rooms.length) {
      var empty = document.createElement("p");
      empty.className = "empty";
      empty.textContent = "No rooms yet.";
      list.appendChild(empty);
      return;
    }
    state.rooms.forEach(function (room) {
      var button = document.createElement("button");
      button.type = "button";
      button.className =
        "room-button" + (room.name === state.room.room ? " selected" : "");
      button.disabled = !(
        state.ready &&
        state.socket &&
        state.socket.readyState === WebSocket.OPEN
      );
      button.dataset.room = room.name;
      var name = document.createElement("span");
      name.textContent = room.name;
      var count = document.createElement("span");
      count.className = "room-count";
      count.textContent = String(
        Number.isFinite(room.members) ? room.members : 0,
      );
      button.append(name, count);
      button.addEventListener("click", function () {
        sendCommand({ type: "join", room: room.name });
      });
      list.appendChild(button);
    });
  }

  function renderRoom() {
    var room = state.room;
    byId("room-title").textContent = room.room ? room.room : "No room selected";
    byId("room-note").textContent = room.room
      ? ""
      : "Join a room to start chatting.";
    byId("member-count").textContent = String(room.members.length);
    renderMembers(room.members);
    renderMessages(room.messages);
    renderRooms();
  }

  function renderMembers(members) {
    var list = byId("member-list");
    list.replaceChildren();
    if (!members.length) {
      var empty = document.createElement("p");
      empty.className = "empty";
      empty.textContent = "No room selected.";
      list.appendChild(empty);
      return;
    }
    members.forEach(function (member) {
      var item = document.createElement("div");
      item.className = "member";
      var dot = document.createElement("span");
      dot.className = "presence" + (member.online ? " online" : "");
      var name = document.createElement("span");
      name.textContent =
        String(member.name || "(unnamed)") +
        (member.online ? "" : " (offline)");
      item.append(dot, name);
      list.appendChild(item);
    });
  }

  function renderMessages(messages) {
    var list = byId("message-list");
    list.replaceChildren();
    if (!messages.length) {
      var empty = document.createElement("p");
      empty.className = "empty";
      empty.textContent = "No messages yet.";
      list.appendChild(empty);
      return;
    }
    messages.forEach(function (message) {
      var item = document.createElement("chat-message");
      item.className = "message";
      item.dataset.messageId = String(message.id);
      var meta = document.createElement("div");
      meta.className = "message-meta";
      meta.textContent = String(message.user || "Unknown");
      var text = document.createElement("div");
      text.className = "message-text";
      text.textContent = String(message.text || "");
      var receipts = document.createElement("chat-read-receipts");
      receipts.receipts = message.receipts || [];
      item.append(meta, text, receipts);
      item.addEventListener("message-read", function (event) {
        if (event.detail) sendCommand({ type: "read", room: state.room.room, messageId: event.detail });
      });
      item.addEventListener("receipts-open", function (event) {
        var popover = document.createElement("chat-receipt-popover");
        popover.receipts = event.detail;
        receipts.appendChild(popover);
        var close = function (click) { if (!popover.contains(click.target) && click.target !== receipts.querySelector("button")) { popover.remove(); document.removeEventListener("click", close); } };
        setTimeout(function () { document.addEventListener("click", close); }, 0);
      });
      list.appendChild(item);
    });
    list.scrollTop = list.scrollHeight;
  }

  function onConnect(event) {
    event.preventDefault();
    var name = byId("name-input").value.trim();
    if (!validName(name)) {
      setError("Use a display name from 1 to 32 characters.");
      return;
    }
    if (state.replaced) removeStorage("tinyactors-chat-resume");
    if (name !== readStorage("tinyactors-chat-name"))
      removeStorage("tinyactors-chat-resume");
    state.name = name;
    state.wantsConnection = true;
    state.retry = 0;
    writeStorage("tinyactors-chat-name", name);
    writeStorage("tinyactors-chat-connect", "1");
    setError("");
    connect(false);
  }

  function onCreate(event) {
    event.preventDefault();
    var input = byId("room-input");
    var room = input.value.trim();
    if (!validRoom(room)) {
      setError("Room names use 1–32 lowercase letters, numbers, _ or -.");
      return;
    }
    if (sendCommand({ type: "create", room: room })) input.value = "";
  }

  function onMessage(event) {
    event.preventDefault();
    var input = byId("message-input");
    var text = input.value.trim();
    if (!text || text.length > 2000) {
      setError("Messages must be between 1 and 2000 characters.");
      return;
    }
    if (sendCommand({ type: "say", text: text })) input.value = "";
  }

  function onRefresh() {
    sendCommand({ type: "list" });
  }
  function onLeave() {
    sendCommand({ type: "leave" });
  }

  function onToggle() {
    if (
      state.socket &&
      (state.socket.readyState === WebSocket.OPEN ||
        state.socket.readyState === WebSocket.CONNECTING)
    ) {
      state.wantsConnection = false;
      writeStorage("tinyactors-chat-connect", "0");
      state.freshNext = false;
      clearRetry();
      state.socket.close(1000, "client disconnect");
      setStatus("offline", "Offline");
      byId("connection-note").textContent =
        "Disconnected. Connect when you are ready.";
      updateControls();
      return;
    }
    var name = byId("name-input").value.trim();
    if (!validName(name)) {
      setError("Use a display name from 1 to 32 characters.");
      return;
    }
    if (name !== readStorage("tinyactors-chat-name"))
      removeStorage("tinyactors-chat-resume");
    state.name = name;
    state.wantsConnection = true;
    state.retry = 0;
    writeStorage("tinyactors-chat-name", name);
    writeStorage("tinyactors-chat-connect", "1");
    connect(state.replaced);
  }

  function start() {
    var savedName = readStorage("tinyactors-chat-name");
    byId("name-input").value = savedName;
    byId("connect-form").addEventListener("submit", onConnect);
    byId("create-form").addEventListener("submit", onCreate);
    byId("message-form").addEventListener("submit", onMessage);
    byId("refresh-rooms").addEventListener("click", onRefresh);
    byId("leave-button").addEventListener("click", onLeave);
    byId("toggle-connection").addEventListener("click", onToggle);
    updateControls();
    if (
      validName(savedName) &&
      readStorage("tinyactors-chat-resume") &&
      readStorage("tinyactors-chat-connect") !== "0"
    ) {
      state.name = savedName;
      state.wantsConnection = true;
      connect(false);
    }
  }

  start();
})();
