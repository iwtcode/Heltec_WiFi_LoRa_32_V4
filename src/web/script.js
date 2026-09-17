const slider = document.getElementById('rpmSlider');
const limitVal = document.getElementById('limitVal');
const tempVal = document.getElementById('tempVal');
const rpmVal = document.getElementById('rpmVal');
const fanIcon = document.getElementById('fanIcon');
const powerToggle = document.getElementById('powerToggle');
const powerStateText = document.getElementById('powerStateText');
const statusDot = document.getElementById('statusDot');
const statusText = document.getElementById('statusText');
const themeToggle = document.getElementById('themeToggle');

const tempMinSlider = document.getElementById('tempMinSlider');
const tempMaxSlider = document.getElementById('tempMaxSlider');
const tempSliderFill = document.getElementById('tempSliderFill');
const tempRangeVal = document.getElementById('tempRangeVal');

const MIN = 5000, MAX = 45000;
let dragging = false, ignoreUpdatesUntil = 0;
let tempDragging = false, tempIgnoreUpdatesUntil = 0;
let ws = null;
let isDark = true;

if (localStorage.getItem('theme') === 'light') {
  isDark = false;
  document.documentElement.setAttribute('data-theme', 'light');
  themeToggle.textContent = '🌙';
}

themeToggle.addEventListener('click', () => {
  isDark = !isDark;
  if(isDark) {
    document.documentElement.removeAttribute('data-theme');
    themeToggle.textContent = '☀️';
    localStorage.setItem('theme', 'dark');
  } else {
    document.documentElement.setAttribute('data-theme', 'light');
    themeToggle.textContent = '🌙';
    localStorage.setItem('theme', 'light');
  }
});

function formatNum(n) { return Math.round(n).toLocaleString('ru-RU'); }

function updateFill() {
  const pct = (slider.value - MIN) / (MAX - MIN) * 100;
  slider.style.setProperty('--fill', pct + '%');
}

function updateTempSliderUI() {
  const minVal = parseInt(tempMinSlider.min);
  const maxVal = parseInt(tempMinSlider.max);
  let min = parseInt(tempMinSlider.value);
  let max = parseInt(tempMaxSlider.value);
  
  const pct1 = ((min - minVal) / (maxVal - minVal)) * 100;
  const pct2 = ((max - minVal) / (maxVal - minVal)) * 100;
  
  tempSliderFill.style.left = pct1 + '%';
  tempSliderFill.style.width = (pct2 - pct1) + '%';
  tempRangeVal.textContent = min + ' - ' + max;
}

function setPowerUI(isOn) {
  powerToggle.checked = isOn;
  powerStateText.textContent = isOn ? 'ВКЛЮЧЕН' : 'ВЫКЛЮЧЕН';
  powerStateText.style.color = isOn ? 'var(--text-main)' : 'var(--text-muted)';
}

function connectWS() {
  ws = new WebSocket(`ws://${location.host}/ws`);

  ws.onopen = () => {
    statusDot.classList.remove('offline');
    statusText.textContent = 'ОНЛАЙН';
  };

  ws.onmessage = (e) => {
    const d = JSON.parse(e.data);

    if (typeof d.power === 'number') setPowerUI(d.power === 1);
    if (typeof d.temp === 'number') tempVal.textContent = d.temp.toFixed(1);

    if (typeof d.current_rpm === 'number') {
      rpmVal.textContent = formatNum(d.current_rpm);
      const speed = Math.max(0.4, Math.min(3, d.current_rpm / MAX * 3));
      fanIcon.style.animationDuration = (d.current_rpm > 50 ? (1 / speed) : 0) + 's';
    }

    if (typeof d.max_rpm === 'number' && !dragging && Date.now() > ignoreUpdatesUntil) {
      slider.value = d.max_rpm;
      limitVal.textContent = formatNum(d.max_rpm);
      updateFill();
    }

    if (typeof d.temp_min === 'number' && typeof d.temp_max === 'number' && !tempDragging && Date.now() > tempIgnoreUpdatesUntil) {
      tempMinSlider.value = Math.round(d.temp_min);
      tempMaxSlider.value = Math.round(d.temp_max);
      updateTempSliderUI();
    }
  };

  ws.onclose = () => {
    statusDot.classList.add('offline');
    statusText.textContent = 'ПОИСК...';
    setTimeout(connectWS, 1500);
  };
}

powerToggle.addEventListener('change', () => {
  const isOn = powerToggle.checked;
  setPowerUI(isOn);
  if (ws && ws.readyState === WebSocket.OPEN) ws.send("power:" + (isOn ? "1" : "0"));
});

slider.addEventListener('input', () => {
  dragging = true;
  limitVal.textContent = formatNum(slider.value);
  updateFill();
});

slider.addEventListener('change', () => {
  dragging = false;
  ignoreUpdatesUntil = Date.now() + 1000;
  if (ws && ws.readyState === WebSocket.OPEN) ws.send("rpm:" + slider.value);
});

tempMinSlider.addEventListener('input', () => {
  let min = parseInt(tempMinSlider.value);
  let max = parseInt(tempMaxSlider.value);
  if(min >= max - 1) tempMinSlider.value = max - 1;
  tempDragging = true;
  updateTempSliderUI();
});

tempMaxSlider.addEventListener('input', () => {
  let min = parseInt(tempMinSlider.value);
  let max = parseInt(tempMaxSlider.value);
  if(max <= min + 1) tempMaxSlider.value = min + 1;
  tempDragging = true;
  updateTempSliderUI();
});

function sendTempRange() {
  tempDragging = false;
  tempIgnoreUpdatesUntil = Date.now() + 1000;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send("temp_range:" + tempMinSlider.value + "," + tempMaxSlider.value);
  }
}

tempMinSlider.addEventListener('change', sendTempRange);
tempMaxSlider.addEventListener('change', sendTempRange);

updateFill();
updateTempSliderUI();
connectWS();