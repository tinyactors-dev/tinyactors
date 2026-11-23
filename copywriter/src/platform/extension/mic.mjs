// mic.html: asks for the microphone in a tab, where Chrome can show its
// prompt. The grant belongs to the extension's origin, so the side panel
// records from then on. The tab closes itself once allowed.

const status = document.getElementById('status');
const allow = document.getElementById('allow');

allow.addEventListener('click', async () => {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    for (const track of stream.getTracks()) track.stop();
    status.textContent = 'Allowed. Hold to talk in the side panel.';
    allow.hidden = true;
    setTimeout(() => window.close(), 1200);
  } catch (e) {
    status.textContent = `Refused (${e.name}). Allow it in the site settings of this extension, then try again.`;
  }
});
