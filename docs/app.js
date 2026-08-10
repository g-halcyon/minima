// Minima Landing Page Interactive Features
document.addEventListener('DOMContentLoaded', () => {
  // Tab Switching Simulator in Browser Showcase
  const tabs = document.querySelectorAll('.tab-item');
  const urlDisplay = document.querySelector('.url-text span');
  const pageTitle = document.querySelector('.card-title');
  const pageContent = document.querySelector('.card-text');
  const shieldCount = document.querySelector('.shield-badge span');

  const tabData = {
    'tab-1': {
      url: 'https://minima.dev',
      title: 'Minima — Ultra-Fast Native Browser',
      content: 'Minima eliminates web framework bloat by running lightweight native C++ code directly against system webviews (WebView2, WebKitGTK, WKWebView, Android WebView).',
      shield: '12 blocked'
    },
    'tab-2': {
      url: 'https://news.mojeek.com',
      title: 'Mojeek Private Search Integration',
      content: 'Minima integrates Mojeek by default — an independent, privacy-first search engine with no tracking, no profiling, and zero user data collection.',
      shield: '8 blocked'
    },
    'tab-3': {
      url: 'https://github.com/g-halcyon/minima',
      title: 'GitHub — g-halcyon/minima',
      content: 'Source-available under PolyForm Noncommercial 1.0 license. Modular single portable C++ core for Windows, Android, Linux, and macOS.',
      shield: '14 blocked'
    }
  };

  tabs.forEach(tab => {
    tab.addEventListener('click', () => {
      tabs.forEach(t => t.classList.remove('active'));
      tab.classList.add('active');

      const dataKey = tab.getAttribute('data-tab');
      if (tabData[dataKey]) {
        urlDisplay.textContent = tabData[dataKey].url;
        pageTitle.textContent = tabData[dataKey].title;
        pageContent.textContent = tabData[dataKey].content;
        shieldCount.textContent = tabData[dataKey].shield;
      }
    });
  });

  // Simulated AI Question Response in Sidebar
  const aiBubble = document.querySelector('.chat-ai');
  const samplePrompts = [
    '✦ Minima AI: This page highlights Minima\'s bare-metal architecture, zero-framework engine, and on-device Gemma AI processing.',
    '✦ Minima AI: Privacy shield active — 12 network tracking scripts intercepted natively.',
    '✦ Minima AI: Minima launches in under 120ms with minimal RAM consumption.'
  ];

  let promptIdx = 0;
  setInterval(() => {
    promptIdx = (promptIdx + 1) % samplePrompts.length;
    if (aiBubble) {
      aiBubble.style.opacity = '0';
      setTimeout(() => {
        aiBubble.textContent = samplePrompts[promptIdx];
        aiBubble.style.opacity = '1';
      }, 300);
    }
  }, 6000);

  // Mobile Menu Toggle
  const menuToggle = document.getElementById('menuToggle');
  const navMenu = document.getElementById('navMenu');

  if (menuToggle && navMenu) {
    menuToggle.addEventListener('click', () => {
      menuToggle.classList.toggle('active');
      navMenu.classList.toggle('active');
    });

    // Close menu when clicking nav link
    const navLinks = navMenu.querySelectorAll('a');
    navLinks.forEach(link => {
      link.addEventListener('click', () => {
        menuToggle.classList.remove('active');
        navMenu.classList.remove('active');
      });
    });
  }
});

