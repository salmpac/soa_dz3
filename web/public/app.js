'use strict';

/*
 * Веб-интерфейс к Booking Service. Без сборки и зависимостей.
 * Все запросы идут на /api/... (nginx проксирует их в booking-service:8080).
 * Данные из API вставляются в DOM только через textContent / атрибуты, без innerHTML.
 */
(function () {
  var API_BASE = '/api';
  var REQUEST_TIMEOUT_MS = 15000;
  var USER_KEY = 'flight-booking.user_id';
  var PASSENGER_KEY = 'flight-booking.passenger';
  // Демо-токен администратора: отправляется в X-Admin-Token на /admin/* (вкладка «Управление рейсами»).
  var ADMIN_TOKEN = 'admin';

  var AIRPORTS = [
    { code: 'SVO', city: 'Москва', name: 'Шереметьево', tz: 'Europe/Moscow' },
    { code: 'VKO', city: 'Москва', name: 'Внуково', tz: 'Europe/Moscow' },
    { code: 'DME', city: 'Москва', name: 'Домодедово', tz: 'Europe/Moscow' },
    { code: 'LED', city: 'Санкт-Петербург', name: 'Пулково', tz: 'Europe/Moscow' },
    { code: 'KZN', city: 'Казань', name: 'Казань', tz: 'Europe/Moscow' },
    { code: 'AER', city: 'Сочи', name: 'Сочи', tz: 'Europe/Moscow' },
    { code: 'OVB', city: 'Новосибирск', name: 'Толмачёво', tz: 'Asia/Novosibirsk' }
  ];
  var AIRPORT_BY_CODE = {};
  AIRPORTS.forEach(function (a) { AIRPORT_BY_CODE[a.code] = a; });

  var QUICK_ROUTES = [
    ['SVO', 'LED'], ['LED', 'SVO'], ['VKO', 'AER'], ['SVO', 'KZN'], ['DME', 'OVB'], ['SVO', 'AER']
  ];

  // ---------------------------------------------------------------------------
  // Утилиты
  // ---------------------------------------------------------------------------

  function $(id) { return document.getElementById(id); }

  /** Создаёт элемент. Строковые дети вставляются как текст (экранируются браузером). */
  function el(tag, attrs) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (key) {
        var value = attrs[key];
        if (value === null || value === undefined || value === false) return;
        if (key === 'class') node.className = value;
        else if (key === 'text') node.textContent = value;
        else if (key.slice(0, 2) === 'on' && typeof value === 'function') node.addEventListener(key.slice(2), value);
        else if (key === 'dataset') Object.keys(value).forEach(function (k) { node.dataset[k] = value[k]; });
        else node.setAttribute(key, value === true ? '' : String(value));
      });
    }
    for (var i = 2; i < arguments.length; i++) append(node, arguments[i]);
    return node;
  }

  function append(node, child) {
    if (child === null || child === undefined || child === false) return;
    if (Array.isArray(child)) { child.forEach(function (c) { append(node, c); }); return; }
    node.appendChild(child instanceof Node ? child : document.createTextNode(String(child)));
  }

  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

  function storageGet(key) {
    try { return window.localStorage.getItem(key); } catch (e) { return null; }
  }
  function storageSet(key, value) {
    try { window.localStorage.setItem(key, value); } catch (e) { /* приватный режим и т.п. */ }
  }

  function randomUserId() {
    var alphabet = 'abcdefghijkmnpqrstuvwxyz23456789';
    var bytes = new Uint8Array(4);
    if (window.crypto && window.crypto.getRandomValues) window.crypto.getRandomValues(bytes);
    else for (var i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
    var s = '';
    for (var j = 0; j < bytes.length; j++) s += alphabet[bytes[j] % alphabet.length];
    return 'user-' + s;
  }

  function plural(n, forms) {
    var a = Math.abs(n) % 100, b = a % 10;
    if (a > 10 && a < 20) return forms[2];
    if (b > 1 && b < 5) return forms[1];
    if (b === 1) return forms[0];
    return forms[2];
  }

  function formatMoney(price, multiplier) {
    if (!price || typeof price.amount_minor !== 'number') return '—';
    var amount = price.amount_minor * (multiplier || 1) / 100;
    var currency = price.currency || 'RUB';
    try {
      return new Intl.NumberFormat('ru-RU', { style: 'currency', currency: currency }).format(amount);
    } catch (e) {
      return amount.toFixed(2).replace('.', ',') + ' ' + currency;
    }
  }

  function airportTz(code) {
    var a = AIRPORT_BY_CODE[String(code || '').toUpperCase()];
    return a ? a.tz : undefined; // неизвестный аэропорт -> часовой пояс браузера
  }

  function airportLabel(code) {
    var a = AIRPORT_BY_CODE[String(code || '').toUpperCase()];
    return a ? a.city + (a.city !== a.name ? ' (' + a.name + ')' : '') : String(code || '');
  }

  function parseDate(iso) {
    var d = new Date(iso);
    return isNaN(d.getTime()) ? null : d;
  }

  function fmt(iso, tz, options) {
    var d = parseDate(iso);
    if (!d) return '—';
    var opts = Object.assign({}, options);
    if (tz) opts.timeZone = tz;
    try { return new Intl.DateTimeFormat('ru-RU', opts).format(d); }
    catch (e) { delete opts.timeZone; return new Intl.DateTimeFormat('ru-RU', opts).format(d); }
  }

  function fmtTime(iso, tz) { return fmt(iso, tz, { hour: '2-digit', minute: '2-digit' }); }
  function fmtDay(iso, tz) { return fmt(iso, tz, { weekday: 'short', day: 'numeric', month: 'short' }); }
  function fmtDateTime(iso) {
    return fmt(iso, undefined, { day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  }

  function fmtDuration(fromIso, toIso) {
    var a = parseDate(fromIso), b = parseDate(toIso);
    if (!a || !b) return '';
    var minutes = Math.round((b - a) / 60000);
    if (minutes <= 0) return '';
    var h = Math.floor(minutes / 60), m = minutes % 60;
    return (h ? h + ' ч' : '') + (h && m ? ' ' : '') + (m ? m + ' мин' : '');
  }

  function todayIso() {
    var d = new Date();
    var pad = function (n) { return (n < 10 ? '0' : '') + n; };
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate());
  }

  // ---------------------------------------------------------------------------
  // API
  // ---------------------------------------------------------------------------

  function ApiError(code, status, message) {
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
    this.message = message || '';
  }
  ApiError.prototype = Object.create(Error.prototype);
  ApiError.prototype.constructor = ApiError;

  function codeFromStatus(status) {
    if (status === 400) return 'INVALID_ARGUMENT';
    if (status === 401) return 'UNAUTHORIZED';
    if (status === 404) return 'NOT_FOUND';
    if (status === 409) return 'FAILED_PRECONDITION';
    if (status === 502) return 'BOOKING_SERVICE_UNAVAILABLE';
    if (status === 503) return 'SERVICE_UNAVAILABLE';
    if (status === 504) return 'GATEWAY_TIMEOUT';
    return 'HTTP_' + status;
  }

  function api(path, options) {
    options = options || {};
    var controller = typeof AbortController === 'function' ? new AbortController() : null;
    var timer = controller ? setTimeout(function () { controller.abort(); }, REQUEST_TIMEOUT_MS) : null;
    var init = {
      method: options.method || 'GET',
      headers: Object.assign({ 'Accept': 'application/json' }, options.headers),
      signal: controller ? controller.signal : undefined,
      cache: 'no-store'
    };
    if (options.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(options.body);
    }

    var status = 0;
    return fetch(API_BASE + path, init)
      .then(function (res) {
        status = res.status;
        return res.text();
      })
      .then(function (text) {
        var data = null;
        if (text) {
          try { data = JSON.parse(text); } catch (e) { data = null; }
        }
        if (status < 200 || status >= 300) {
          var err = data && data.error;
          throw new ApiError(
            (err && err.code) || codeFromStatus(status),
            status,
            (err && typeof err.message === 'string') ? err.message : '');
        }
        if (data === null && text) throw new ApiError('BAD_RESPONSE', status, 'Сервер вернул некорректный ответ');
        return data;
      })
      .catch(function (e) {
        if (e instanceof ApiError) throw e;
        if (e && e.name === 'AbortError') throw new ApiError('CLIENT_TIMEOUT', 0, '');
        throw new ApiError('NETWORK', 0, e && e.message ? e.message : '');
      })
      .finally(function () { if (timer) clearTimeout(timer); });
  }

  var FRIENDLY = {
    NOT_ENOUGH_SEATS: 'Недостаточно свободных мест на этом рейсе. Уменьшите количество мест или выберите другой рейс.',
    FLIGHT_SERVICE_UNAVAILABLE: 'Сервис рейсов недоступен, попробуйте позже.',
    FLIGHT_SERVICE_TIMEOUT: 'Сервис рейсов не ответил вовремя, попробуйте позже.',
    BOOKING_SERVICE_UNAVAILABLE: 'Сервис бронирования недоступен, попробуйте позже.',
    SERVICE_UNAVAILABLE: 'Сервис временно недоступен, попробуйте позже.',
    GATEWAY_TIMEOUT: 'Сервер не ответил вовремя, попробуйте позже.',
    CLIENT_TIMEOUT: 'Сервер не ответил вовремя. Проверьте соединение и попробуйте ещё раз.',
    NETWORK: 'Нет связи с сервером. Проверьте подключение к сети и попробуйте ещё раз.',
    INVALID_ARGUMENT: 'Проверьте введённые данные.',
    NOT_FOUND: 'Не найдено.',
    FAILED_PRECONDITION: 'Операция сейчас невозможна.',
    ALREADY_EXISTS: 'Такая запись уже существует.',
    UNAUTHORIZED: 'Нет доступа: требуется авторизация.',
    INTERNAL: 'Внутренняя ошибка сервера. Попробуйте позже.',
    BAD_RESPONSE: 'Сервер вернул некорректный ответ.'
  };

  /**
   * -> { title, detail }: понятный русский текст + сообщение сервера (error.message) и код.
   * overrides — необязательная карта { CODE: 'текст' } для контекста (например, админка).
   */
  function describeError(err, overrides) {
    var code = (err && err.code) || '';
    var msg = (err && err.message) || '';
    var friendly = (overrides && overrides[code]) || FRIENDLY[code];
    if (code === 'NETWORK' || code === 'CLIENT_TIMEOUT') return { title: friendly, detail: '' };
    if (friendly) return { title: friendly, detail: msg ? msg + ' (' + code + ')' : '' };
    if (msg) return { title: msg, detail: code };
    return { title: err && err.status >= 500 ? 'Ошибка сервера. Попробуйте позже.' : 'Что-то пошло не так.', detail: code };
  }

  function simpleError(text) {
    return el('div', { class: 'alert alert--error', role: 'alert', text: text });
  }

  function errorAlert(err, retry, overrides) {
    var d = describeError(err, overrides);
    return el('div', { class: 'alert alert--error', role: 'alert' },
      el('div', { class: 'alert__title', text: d.title }),
      d.detail ? el('div', { class: 'alert__detail', text: d.detail }) : null,
      retry ? el('button', { type: 'button', class: 'btn btn--ghost btn--small', onclick: retry, text: 'Повторить' }) : null);
  }

  // Кеш рейсов для списка бронирований (промисы, чтобы не дублировать запросы).
  var flightCache = new Map();
  function getFlight(id, fresh) {
    var key = String(id);
    if (!fresh && flightCache.has(key)) return flightCache.get(key);
    var p = api('/flights/' + encodeURIComponent(key));
    flightCache.set(key, p);
    p.catch(function () { flightCache.delete(key); });
    return p;
  }

  // ---------------------------------------------------------------------------
  // Общие элементы UI
  // ---------------------------------------------------------------------------

  /** Блокирует кнопку на время операции, показывает спиннер. Возвращает функцию разблокировки. */
  function busy(button, label) {
    var original = button.textContent;
    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    if (label) button.textContent = label;
    return function () {
      button.disabled = false;
      button.removeAttribute('aria-busy');
      button.textContent = original;
    };
  }

  function toast(text) {
    var node = el('div', { class: 'toast', text: text });
    $('toasts').appendChild(node);
    setTimeout(function () { node.remove(); }, 4000);
  }

  function setStatus(container, content) {
    clear(container);
    append(container, content);
  }

  function skeletons(list, n) {
    clear(list);
    for (var i = 0; i < n; i++) list.appendChild(el('li', { class: 'skeleton', 'aria-hidden': 'true' }));
  }

  function statusBadge(status) {
    var s = String(status || '');
    var map = { CONFIRMED: ['Подтверждено', 'badge--confirmed'], CANCELLED: ['Отменено', 'badge--cancelled'] };
    var m = map[s] || [s || '—', 'badge--other'];
    return el('span', { class: 'badge ' + m[1], text: m[0] });
  }

  // ---------------------------------------------------------------------------
  // Текущий пользователь
  // ---------------------------------------------------------------------------

  var currentUser = storageGet(USER_KEY);
  if (!currentUser) {
    currentUser = randomUserId();
    storageSet(USER_KEY, currentUser);
  }

  function initUser() {
    var input = $('user-id');
    input.value = currentUser;
    function commit() {
      var v = input.value.trim();
      if (!v) { input.value = currentUser; return; }
      if (v === currentUser) return;
      currentUser = v;
      storageSet(USER_KEY, v);
      toast('Текущий пользователь: ' + v);
      $('bookings-user').textContent = v;
      bookingsLoaded = false;
      if (currentTab === 'bookings') loadBookings();
    }
    input.addEventListener('change', commit);
    $('user-form').addEventListener('submit', function (e) { e.preventDefault(); commit(); input.blur(); });
    $('bookings-user').textContent = currentUser;
  }

  // ---------------------------------------------------------------------------
  // Вкладки (#search, #bookings, #lookup)
  // ---------------------------------------------------------------------------

  var currentTab = 'search';
  var bookingsLoaded = false;

  function showTab() {
    var tab = (location.hash || '#search').slice(1);
    if (['search', 'bookings', 'lookup', 'admin'].indexOf(tab) === -1) tab = 'search';
    currentTab = tab;
    document.querySelectorAll('[data-tab-panel]').forEach(function (p) {
      p.hidden = p.getAttribute('data-tab-panel') !== tab;
    });
    document.querySelectorAll('.tabs__item').forEach(function (a) {
      if (a.getAttribute('data-tab') === tab) a.setAttribute('aria-current', 'page');
      else a.removeAttribute('aria-current');
    });
    if (tab === 'bookings' && !bookingsLoaded) loadBookings();
    if (tab === 'admin' && !adminRefsLoaded) loadAdminRefs();
  }

  // ---------------------------------------------------------------------------
  // Поиск рейсов
  // ---------------------------------------------------------------------------

  var lastSearch = null;
  var searching = false;
  var searchSeq = 0;
  var flightsById = new Map();

  function initSearch() {
    var datalist = $('airports');
    AIRPORTS.forEach(function (a) {
      datalist.appendChild(el('option', { value: a.code, label: a.city + ', ' + a.name }));
    });

    var quick = $('quick-routes');
    QUICK_ROUTES.forEach(function (r) {
      quick.appendChild(el('button', {
        type: 'button', class: 'chip', text: r[0] + ' → ' + r[1],
        onclick: function () {
          $('origin').value = r[0];
          $('destination').value = r[1];
          updateHint('origin'); updateHint('destination');
          submitSearch();
        }
      }));
    });

    ['origin', 'destination'].forEach(function (id) {
      var input = $(id);
      input.addEventListener('input', function () {
        input.value = input.value.toUpperCase().replace(/[^A-Z]/g, '').slice(0, 3);
        input.removeAttribute('aria-invalid');
        updateHint(id);
      });
    });

    $('swap').addEventListener('click', function () {
      var o = $('origin'), d = $('destination'), t = o.value;
      o.value = d.value; d.value = t;
      updateHint('origin'); updateHint('destination');
    });

    var date = $('date');
    date.min = todayIso();
    date.addEventListener('input', function () { $('date-clear').hidden = !date.value; });
    $('date-clear').addEventListener('click', function () { date.value = ''; $('date-clear').hidden = true; });

    $('search-form').addEventListener('submit', function (e) { e.preventDefault(); submitSearch(); });

    setStatus($('search-status'), el('div', { class: 'empty', text: 'Выберите направление и нажмите «Найти рейсы».' }));
  }

  function updateHint(id) {
    var hint = document.querySelector('[data-hint-for="' + id + '"]');
    var code = $(id).value;
    hint.classList.remove('is-error');
    hint.textContent = AIRPORT_BY_CODE[code] ? airportLabel(code) : '';
  }

  function setFieldError(id, message) {
    var hint = document.querySelector('[data-hint-for="' + id + '"]');
    $(id).setAttribute('aria-invalid', 'true');
    hint.classList.add('is-error');
    hint.textContent = message;
  }

  function submitSearch() {
    var origin = $('origin').value.trim().toUpperCase();
    var destination = $('destination').value.trim().toUpperCase();
    var date = $('date').value;
    var ok = true;
    if (!/^[A-Z]{3}$/.test(origin)) { setFieldError('origin', 'Укажите код аэропорта из 3 букв'); ok = false; }
    if (!/^[A-Z]{3}$/.test(destination)) { setFieldError('destination', 'Укажите код аэропорта из 3 букв'); ok = false; }
    if (ok && origin === destination) { setFieldError('destination', 'Пункты вылета и прилёта совпадают'); ok = false; }
    if (!ok) return;
    runSearch({ origin: origin, destination: destination, date: date }, false);
  }

  /** silent=true — тихое обновление (после бронирования): список не прячется за скелетоном. */
  function runSearch(params, silent) {
    if (searching) return; // идёт поиск — не мешаем ему (в т.ч. тихим обновлением)
    var seq = ++searchSeq;
    var release = silent ? null : busy($('search-submit'), 'Ищем…');
    searching = !silent;
    var list = $('flight-list');
    var status = $('search-status');
    if (!silent) {
      setStatus(status, null);
      skeletons(list, 3);
    }

    var qs = 'origin=' + encodeURIComponent(params.origin) + '&destination=' + encodeURIComponent(params.destination);
    if (params.date) qs += '&date=' + encodeURIComponent(params.date);

    return api('/flights?' + qs)
      .then(function (flights) {
        if (seq !== searchSeq) return;
        lastSearch = params;
        renderFlights(Array.isArray(flights) ? flights : [], params);
      })
      .catch(function (err) {
        if (seq !== searchSeq) return;
        if (silent) return; // тихое обновление остатка мест — best effort
        clear(list);
        setStatus(status, errorAlert(err, function () { runSearch(params, false); }));
      })
      .finally(function () {
        if (release) { release(); searching = false; }
      });
  }

  function renderFlights(flights, params) {
    var list = $('flight-list');
    var status = $('search-status');
    clear(list);
    flightsById.clear();

    var route = params.origin + ' → ' + params.destination;
    if (!flights.length) {
      setStatus(status, el('div', { class: 'empty' },
        el('div', { text: 'Рейсов ' + route + (params.date ? ' на ' + params.date : '') + ' не найдено.' }),
        el('div', { class: 'muted', text: 'Попробуйте другую дату или направление.' })));
      return;
    }

    setStatus(status, 'Найдено ' + flights.length + ' ' + plural(flights.length, ['рейс', 'рейса', 'рейсов']) +
      ' · ' + route + (params.date ? ' · ' + params.date : '') + ' · время местное');

    flights.forEach(function (f) {
      flightsById.set(String(f.id), f);
      list.appendChild(flightCard(f));
    });
  }

  function seatsInfo(f) {
    var avail = Number(f.available_seats), total = Number(f.total_seats);
    var cls = 'seats';
    var text;
    if (!(avail > 0)) { cls += ' seats--none'; text = 'Мест нет'; }
    else {
      if (avail <= 5) cls += ' seats--low';
      text = 'Свободно ' + avail + ' из ' + total;
    }
    return el('span', { class: cls, text: text });
  }

  /**
   * Карточка рейса. opts (необязательно):
   *   actions — узлы вместо кнопки «Забронировать»;
   *   head / extra — содержимое во всю ширину карточки сверху / снизу;
   *   showStatus — показать статус рейса (для админки).
   */
  function flightCard(f, opts) {
    opts = opts || {};
    var depTz = airportTz(f.origin), arrTz = airportTz(f.destination);
    var depDay = fmtDay(f.departure_time, depTz), arrDay = fmtDay(f.arrival_time, arrTz);
    var duration = fmtDuration(f.departure_time, f.arrival_time);
    var soldOut = !(Number(f.available_seats) > 0);
    var dep = parseDate(f.departure_time);
    var departed = dep && dep.getTime() < Date.now();

    var inactive = opts.showStatus && f.status && f.status !== 'SCHEDULED';
    var actions = opts.actions
      ? el('div', { class: 'flight__actions' }, opts.actions)
      : el('button', {
        type: 'button',
        class: 'btn btn--primary btn--small',
        disabled: soldOut || departed,
        text: departed ? 'Рейс вылетел' : soldOut ? 'Нет мест' : 'Забронировать',
        onclick: function () { openBooking(flightsById.get(String(f.id)) || f); }
      });

    return el('li', { class: 'card flight' + (inactive ? ' is-inactive' : ''), dataset: { flightId: String(f.id) } },
      opts.head ? el('div', { class: 'flight__full flight-head' }, opts.head) : null,
      el('div', null,
        el('div', { class: 'flight__times' },
          el('div', { class: 'flight__point' },
            el('span', { class: 'flight__time', text: fmtTime(f.departure_time, depTz) }),
            el('span', { class: 'flight__code', text: f.origin, title: airportLabel(f.origin) })),
          el('div', { class: 'flight__line', text: duration || ' ' }),
          el('div', { class: 'flight__point' },
            el('span', { class: 'flight__time', text: fmtTime(f.arrival_time, arrTz) }),
            el('span', { class: 'flight__code', text: f.destination, title: airportLabel(f.destination) }))),
        el('div', { class: 'flight__meta' },
          el('span', null, el('strong', { text: f.flight_number }), ' · ', f.airline_name || f.airline_code || ''),
          el('span', { text: depDay + (arrDay !== depDay ? ' → ' + arrDay : '') }),
          el('span', { text: airportLabel(f.origin) + ' → ' + airportLabel(f.destination) }))),
      el('div', { class: 'flight__side' },
        el('span', { class: 'price', text: formatMoney(f.price) }),
        seatsInfo(f),
        actions),
      opts.extra ? el('div', { class: 'flight__full' }, opts.extra) : null);
  }

  function flightSummary(f) {
    var depTz = airportTz(f.origin), arrTz = airportTz(f.destination);
    return [
      el('div', { class: 'route', text: f.origin + ' → ' + f.destination + ' · ' + f.flight_number }),
      el('div', { text: (f.airline_name || f.airline_code || '') }),
      el('div', {
        class: 'muted',
        text: fmtDay(f.departure_time, depTz) + ', ' + fmtTime(f.departure_time, depTz) + ' – ' +
          fmtTime(f.arrival_time, arrTz) + (fmtDuration(f.departure_time, f.arrival_time) ? ' (' + fmtDuration(f.departure_time, f.arrival_time) + ')' : '')
      })
    ];
  }

  // ---------------------------------------------------------------------------
  // Бронирование
  // ---------------------------------------------------------------------------

  var bookingFlight = null;
  var bookingInFlight = false;

  function initBookingDialog() {
    var dialog = $('book-dialog');
    dialog.querySelectorAll('[data-close]').forEach(function (b) {
      b.addEventListener('click', function () { closeDialog(); });
    });
    dialog.addEventListener('cancel', function (e) { if (bookingInFlight) e.preventDefault(); });
    dialog.addEventListener('click', function (e) {
      if (e.target === dialog && !bookingInFlight) closeDialog(); // клик по подложке
    });
    $('seat-count').addEventListener('input', updateTotal);
    $('book-form').addEventListener('submit', function (e) { e.preventDefault(); submitBooking(); });
    $('book-go-bookings').addEventListener('click', function () {
      closeDialog();
      location.hash = '#bookings';
    });
  }

  function closeDialog() {
    if (bookingInFlight) return;
    var dialog = $('book-dialog');
    if (dialog.open) dialog.close();
  }

  function openBooking(f) {
    bookingFlight = f;
    clear($('book-flight'));
    append($('book-flight'), flightSummary(f));
    $('book-form').hidden = false;
    $('book-result').hidden = true;
    $('book-error').hidden = true;
    clear($('book-error'));

    var saved = null;
    try { saved = JSON.parse(storageGet(PASSENGER_KEY) || 'null'); } catch (e) { saved = null; }
    if (saved && !$('passenger-name').value) $('passenger-name').value = saved.name || '';
    if (saved && !$('passenger-email').value) $('passenger-email').value = saved.email || '';
    var seats = $('seat-count');
    seats.max = String(Math.max(1, Math.min(50, Number(f.available_seats) || 1)));
    seats.value = '1';
    updateTotal();

    var dialog = $('book-dialog');
    if (typeof dialog.showModal === 'function') dialog.showModal();
    else dialog.setAttribute('open', '');
    ($('passenger-name').value ? seats : $('passenger-name')).focus();
  }

  function updateTotal() {
    var n = parseInt($('seat-count').value, 10);
    var f = bookingFlight;
    $('book-total').textContent = f && n > 0 ? formatMoney(f.price, n) : '—';
    var hint = $('seat-hint');
    var avail = f ? Number(f.available_seats) : 0;
    hint.classList.remove('is-error');
    hint.textContent = f ? 'Свободно мест: ' + avail + '. За одно бронирование — от 1 до 50.' : '';
    if (f && n > avail) {
      hint.classList.add('is-error');
      hint.textContent = 'На рейсе осталось только ' + avail + ' ' + plural(avail, ['место', 'места', 'мест']) + '.';
    }
  }

  function showBookingError(content) {
    var box = $('book-error');
    clear(box);
    append(box, content);
    box.hidden = false;
  }

  function submitBooking() {
    if (bookingInFlight || !bookingFlight) return;
    var name = $('passenger-name').value.trim();
    var email = $('passenger-email').value.trim();
    var seatCount = Number($('seat-count').value);
    $('book-error').hidden = true;

    if (!name) { showBookingError(simpleError('Укажите имя пассажира.')); $('passenger-name').focus(); return; }
    if (!email || !$('passenger-email').checkValidity()) { showBookingError(simpleError('Укажите корректный email.')); $('passenger-email').focus(); return; }
    if (!Number.isInteger(seatCount) || seatCount < 1 || seatCount > 50) {
      showBookingError(simpleError('Количество мест — целое число от 1 до 50.')); $('seat-count').focus(); return;
    }

    bookingInFlight = true;
    var release = busy($('book-submit'), 'Бронируем…');
    var flight = bookingFlight;
    storageSet(PASSENGER_KEY, JSON.stringify({ name: name, email: email }));

    api('/bookings', {
      method: 'POST',
      body: {
        user_id: currentUser,
        flight_id: flight.id,
        passenger_name: name,
        passenger_email: email,
        seat_count: seatCount
      }
    })
      .then(function (booking) {
        showBookingResult(booking, flight);
        bookingsLoaded = false;
        flightCache.delete(String(flight.id));
        if (lastSearch) runSearch(lastSearch, true); // обновить остаток мест
      })
      .catch(function (err) {
        showBookingError(errorAlert(err));
        if (err.code === 'NOT_ENOUGH_SEATS') {
          // Обновим остаток мест в диалоге и в списке.
          getFlight(flight.id, true).then(function (fresh) {
            if (bookingFlight && String(bookingFlight.id) === String(fresh.id)) {
              bookingFlight = fresh;
              updateTotal();
            }
          }).catch(function () {});
          if (lastSearch) runSearch(lastSearch, true);
        }
      })
      .finally(function () {
        bookingInFlight = false;
        release();
      });
  }

  function showBookingResult(b, flight) {
    $('book-form').hidden = true;
    var dl = $('book-result-details');
    clear(dl);
    var rows = [
      ['Номер', el('code', { text: b.id })],
      ['Статус', statusBadge(b.status)],
      ['Рейс', flight.flight_number + ', ' + flight.origin + ' → ' + flight.destination],
      ['Пассажир', b.passenger_name + ' <' + b.passenger_email + '>'],
      ['Мест', String(b.seat_count)],
      ['Итого', el('strong', { text: formatMoney(b.total_price) })]
    ];
    rows.forEach(function (r) { dl.appendChild(el('dt', { text: r[0] })); dl.appendChild(el('dd', null, r[1])); });
    $('book-result').hidden = false;
    $('book-go-bookings').focus();
  }

  // ---------------------------------------------------------------------------
  // Карточка бронирования (общая для «Мои бронирования» и поиска по id)
  // ---------------------------------------------------------------------------

  function bookingCard(b, onChange) {
    var li = el('li', { class: 'card booking' + (b.status === 'CANCELLED' ? ' is-cancelled' : '') });

    var flightLine = el('div', { class: 'booking__flight' }, el('span', { class: 'muted', text: 'Рейс #' + b.flight_id + ' · загрузка…' }));
    getFlight(b.flight_id).then(function (f) {
      clear(flightLine);
      var depTz = airportTz(f.origin), arrTz = airportTz(f.destination);
      append(flightLine, [
        f.flight_number + ' · ' + f.origin + ' → ' + f.destination + ' ',
        el('span', {
          class: 'muted',
          text: '· ' + fmtDay(f.departure_time, depTz) + ', ' + fmtTime(f.departure_time, depTz) + ' – ' + fmtTime(f.arrival_time, arrTz)
        })
      ]);
    }).catch(function (err) {
      clear(flightLine);
      append(flightLine, el('span', {
        class: 'muted',
        text: 'Рейс #' + b.flight_id + ' — информация недоступна (' + describeError(err).title.replace(/\.$/, '') + ')'
      }));
    });

    var actions = el('div', { class: 'booking__actions' });

    function renderActions() {
      clear(actions);
      if (b.status !== 'CONFIRMED') return;
      actions.appendChild(el('button', {
        type: 'button', class: 'btn btn--danger-ghost btn--small', text: 'Отменить',
        onclick: renderConfirm
      }));
    }

    function renderConfirm() {
      clear(actions);
      var no = el('button', { type: 'button', class: 'btn btn--ghost btn--small', text: 'Нет', onclick: renderActions });
      var yes = el('button', { type: 'button', class: 'btn btn--danger btn--small', text: 'Да, отменить' });
      yes.addEventListener('click', function () {
        if (yes.disabled) return;
        no.disabled = true;
        var release = busy(yes, 'Отменяем…');
        api('/bookings/' + encodeURIComponent(b.id) + '/cancel', { method: 'POST' })
          .then(function (updated) {
            toast('Бронирование отменено');
            flightCache.delete(String(b.flight_id));
            if (lastSearch) runSearch(lastSearch, true); // места вернулись в продажу
            onChange(updated || Object.assign({}, b, { status: 'CANCELLED' }));
          })
          .catch(function (err) {
            release();
            no.disabled = false;
            clear(actions);
            actions.appendChild(errorAlert(err));
            if (err.code === 'FAILED_PRECONDITION' || err.code === 'NOT_FOUND') {
              // Скорее всего, статус уже изменился — перечитаем бронирование.
              api('/bookings/' + encodeURIComponent(b.id)).then(function (fresh) {
                toast(describeError(err).title + ' ' + (err.message || ''));
                onChange(fresh);
              }).catch(function () {});
            } else {
              actions.appendChild(el('button', { type: 'button', class: 'btn btn--ghost btn--small', text: 'Назад', onclick: renderActions }));
            }
          });
      });
      append(actions, [el('span', { class: 'booking__confirm', text: 'Отменить бронирование?' }), no, yes]);
      no.focus();
    }

    renderActions();

    var details = el('dl', { class: 'kv' },
      el('dt', { text: 'Пассажир' }), el('dd', { text: b.passenger_name }),
      el('dt', { text: 'Email' }), el('dd', { text: b.passenger_email }),
      el('dt', { text: 'Мест' }), el('dd', { text: String(b.seat_count) }),
      el('dt', { text: 'Сумма' }), el('dd', null, el('strong', { text: formatMoney(b.total_price) })),
      el('dt', { text: 'Создано' }), el('dd', { text: fmtDateTime(b.created_at) }),
      b.status === 'CANCELLED' && b.updated_at ? [el('dt', { text: 'Отменено' }), el('dd', { text: fmtDateTime(b.updated_at) })] : null,
      b.user_id !== currentUser ? [el('dt', { text: 'Пользователь' }), el('dd', null, el('code', { text: b.user_id }))] : null);

    append(li, [
      el('div', { class: 'booking__head' },
        el('span', { class: 'booking__id', text: '№ ' + b.id }),
        statusBadge(b.status)),
      flightLine,
      el('div', { class: 'booking__body' }, details, actions)
    ]);
    return li;
  }

  // ---------------------------------------------------------------------------
  // Мои бронирования
  // ---------------------------------------------------------------------------

  var loadingBookings = false;

  function initBookings() {
    $('bookings-refresh').addEventListener('click', function () { loadBookings(); });
  }

  function loadBookings() {
    if (loadingBookings) return;
    loadingBookings = true;
    var user = currentUser;
    var list = $('booking-list');
    var status = $('bookings-status');
    var release = busy($('bookings-refresh'), 'Загрузка…');
    setStatus(status, null);
    skeletons(list, 2);

    api('/bookings?user_id=' + encodeURIComponent(user))
      .then(function (bookings) {
        if (user !== currentUser) return;
        bookingsLoaded = true;
        clear(list);
        bookings = Array.isArray(bookings) ? bookings : [];
        if (!bookings.length) {
          setStatus(status, el('div', { class: 'empty' },
            el('div', { text: 'У пользователя пока нет бронирований.' }),
            el('a', { href: '#search', text: 'Найти рейс' })));
          return;
        }
        var active = bookings.filter(function (b) { return b.status === 'CONFIRMED'; }).length;
        setStatus(status, 'Всего ' + bookings.length + ' · активных ' + active);
        bookings.forEach(function (b) { list.appendChild(renderBookingItem(b)); });
      })
      .catch(function (err) {
        if (user !== currentUser) return;
        clear(list);
        setStatus(status, errorAlert(err, loadBookings));
      })
      .finally(function () {
        loadingBookings = false;
        release();
        if (user !== currentUser) loadBookings(); // пользователь сменился во время загрузки
      });
  }

  function renderBookingItem(b) {
    var node = bookingCard(b, function (updated) {
      var fresh = renderBookingItem(updated);
      node.replaceWith(fresh);
    });
    return node;
  }

  // ---------------------------------------------------------------------------
  // Поиск бронирования по id
  // ---------------------------------------------------------------------------

  var lookingUp = false;

  function initLookup() {
    $('lookup-form').addEventListener('submit', function (e) {
      e.preventDefault();
      if (lookingUp) return;
      var id = $('lookup-id').value.trim();
      var status = $('lookup-status');
      var list = $('lookup-result');
      if (!id) {
        setStatus(status, simpleError('Введите номер бронирования.'));
        $('lookup-id').focus();
        return;
      }
      lookingUp = true;
      var release = busy($('lookup-submit'), 'Ищем…');
      setStatus(status, null);
      clear(list);
      api('/bookings/' + encodeURIComponent(id))
        .then(function (b) {
          list.appendChild(renderLookupItem(b));
        })
        .catch(function (err) {
          if (err.code === 'NOT_FOUND') {
            setStatus(status, el('div', { class: 'alert alert--error', role: 'alert' },
              el('div', { class: 'alert__title', text: 'Бронирование не найдено.' }),
              err.message ? el('div', { class: 'alert__detail', text: err.message }) : null));
          } else {
            setStatus(status, errorAlert(err));
          }
        })
        .finally(function () { lookingUp = false; release(); });
    });
  }

  function renderLookupItem(b) {
    var node = bookingCard(b, function (updated) {
      bookingsLoaded = false;
      node.replaceWith(renderLookupItem(updated));
    });
    return node;
  }

  // ---------------------------------------------------------------------------
  // Управление рейсами (администратор)
  // ---------------------------------------------------------------------------

  var ADMIN_ERRORS = {
    UNAUTHORIZED: 'Неверный токен администратора — доступ запрещён.',
    ALREADY_EXISTS: 'Такой рейс в эту дату уже есть.',
    INVALID_ARGUMENT: 'Сервер отклонил данные рейса.',
    NOT_FOUND: 'Рейс не найден.'
  };

  var FLIGHT_STATUSES = [
    ['SCHEDULED', 'Запланирован'],
    ['DEPARTED', 'Вылетел'],
    ['CANCELLED', 'Отменён'],
    ['COMPLETED', 'Завершён']
  ];

  var MAX_INT32 = 2147483647;

  function adminApi(path, options) {
    options = Object.assign({}, options);
    options.headers = Object.assign({}, options.headers, { 'X-Admin-Token': ADMIN_TOKEN });
    return api(path, options);
  }

  function flightStatusBadge(status) {
    var s = String(status || '');
    var cls = { SCHEDULED: 'badge--scheduled', DEPARTED: 'badge--departed', CANCELLED: 'badge--cancelled' }[s] || 'badge--other';
    var label = s || '—';
    FLIGHT_STATUSES.forEach(function (p) { if (p[0] === s) label = p[1]; });
    return el('span', { class: 'badge ' + cls, text: label });
  }

  /** Смещение часового пояса tz (минуты к востоку от UTC) в момент utcMs. tz не задан -> пояс браузера. */
  function tzOffsetMinutes(tz, utcMs) {
    if (!tz) return -new Date(utcMs).getTimezoneOffset();
    var parts = new Intl.DateTimeFormat('en-US', {
      timeZone: tz, hourCycle: 'h23',
      year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit'
    }).formatToParts(new Date(utcMs));
    var v = {};
    parts.forEach(function (p) { v[p.type] = p.value; });
    var wall = Date.UTC(+v.year, +v.month - 1, +v.day, (+v.hour) % 24, +v.minute, +v.second);
    return Math.round((wall - utcMs) / 60000);
  }

  function utcOffsetLabel(minutes) {
    var sign = minutes < 0 ? '−' : '+';
    var a = Math.abs(minutes), h = Math.floor(a / 60), m = a % 60;
    return 'UTC' + sign + h + (m ? ':' + (m < 10 ? '0' : '') + m : '');
  }

  /**
   * Местные дата ('YYYY-MM-DD') и время ('HH:MM') в поясе tz -> Date (UTC-момент).
   * null — пустой/некорректный ввод; { gap: true } — такого местного времени нет (перевод часов).
   */
  function localToUtc(dateStr, timeStr, tz) {
    var dm = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateStr || '');
    var tm = /^(\d{2}):(\d{2})(?::\d{2}(?:\.\d+)?)?$/.exec(timeStr || '');
    if (!dm || !tm) return null;
    var wall = Date.UTC(+dm[1], +dm[2] - 1, +dm[3], +tm[1], +tm[2]);
    if (isNaN(wall)) return null;
    var off = tzOffsetMinutes(tz, wall);
    var utc = wall - off * 60000;
    var off2 = tzOffsetMinutes(tz, utc);
    if (off2 !== off) utc = wall - off2 * 60000;
    if (utc + tzOffsetMinutes(tz, utc) * 60000 !== wall) return { gap: true };
    return new Date(utc);
  }

  function isoZ(date) { return date.toISOString().replace(/\.\d{3}Z$/, 'Z'); }

  /** '5 500,50' / '5500.5' / '5500' -> копейки (целое) без плавающей точки; иначе null. */
  function parseRublesToMinor(text) {
    var s = String(text || '').replace(/[\s\u00a0\u202f']/g, '').replace(/₽$/, '').replace(',', '.');
    var m = /^(\d{1,13})(?:\.(\d{0,2}))?$/.exec(s);
    if (!m) return null;
    var minor = Number(m[1]) * 100 + Number(((m[2] || '') + '00').slice(0, 2));
    return Number.isSafeInteger(minor) ? minor : null;
  }

  function minorToInput(amountMinor) {
    var a = Number(amountMinor);
    if (!Number.isSafeInteger(a) || a < 0) return '';
    var r = a % 100;
    return Math.floor(a / 100) + ',' + (r < 10 ? '0' : '') + r;
  }

  function parsePositiveInt(text, max) {
    var s = String(text || '').trim();
    if (!/^\d{1,10}$/.test(s)) return null;
    var n = Number(s);
    return n >= 1 && n <= max ? n : null;
  }

  // --- подсказки/ошибки полей (data-hint-for) ---

  function adminHint(id) { return document.querySelector('[data-hint-for="' + id + '"]'); }

  function adminFieldError(id, message, inputs) {
    var hint = adminHint(id);
    (inputs || [id]).forEach(function (i) { $(i).setAttribute('aria-invalid', 'true'); });
    hint.classList.add('is-error');
    hint.textContent = message;
  }

  function adminFieldReset(id, inputs, text) {
    var hint = adminHint(id);
    (inputs || [id]).forEach(function (i) { $(i).removeAttribute('aria-invalid'); });
    hint.classList.remove('is-error');
    hint.textContent = text !== undefined ? text : (hint.dataset.default || '');
  }

  // --- справочники ---

  var adminRefsLoaded = false;
  var adminRefsLoading = false;
  var adminAirports = {};   // iata_code -> { iata_code, name, city }
  var adminAirlines = {};   // code -> { code, name }

  function fillSelect(select, items, placeholder) {
    clear(select);
    select.appendChild(el('option', { value: '', text: placeholder }));
    items.forEach(function (it) { select.appendChild(el('option', { value: it[0], text: it[1] })); });
  }

  function loadAdminRefs() {
    if (adminRefsLoading || adminRefsLoaded) return;
    adminRefsLoading = true;
    var status = $('admin-ref-status');
    setStatus(status, el('span', { class: 'muted', text: 'Загружаем справочники аэропортов и авиакомпаний…' }));
    Promise.all([adminApi('/admin/airports'), adminApi('/admin/airlines')])
      .then(function (res) {
        var airports = Array.isArray(res[0]) ? res[0] : [];
        var airlines = Array.isArray(res[1]) ? res[1] : [];
        if (!airports.length || !airlines.length) {
          throw new ApiError('BAD_RESPONSE', 200, 'Пустой справочник аэропортов или авиакомпаний');
        }
        adminAirports = {}; adminAirlines = {};
        airports.forEach(function (a) { adminAirports[String(a.iata_code)] = a; });
        airlines.forEach(function (a) { adminAirlines[String(a.code)] = a; });

        var airportItems = airports.map(function (a) {
          return [String(a.iata_code), a.iata_code + ' — ' + a.name + (a.city ? ', ' + a.city : '')];
        });
        fillSelect($('cf-airline'), airlines.map(function (a) { return [String(a.code), a.code + ' — ' + a.name]; }), 'Выберите авиакомпанию');
        fillSelect($('cf-origin'), airportItems, 'Аэропорт вылета');
        fillSelect($('cf-destination'), airportItems, 'Аэропорт прилёта');
        ['cf-airline', 'cf-origin', 'cf-destination'].forEach(function (id) { $(id).disabled = false; });
        $('create-submit').disabled = false;
        adminRefsLoaded = true;
        setStatus(status, null);
        syncAirportSelects();
        updateCreatePreview();
      })
      .catch(function (err) {
        ['cf-airline', 'cf-origin', 'cf-destination'].forEach(function (id) {
          fillSelect($(id), [], 'Нет данных');
          $(id).disabled = true;
        });
        $('create-submit').disabled = true;
        setStatus(status, errorAlert(err, loadAdminRefs, ADMIN_ERRORS));
      })
      .finally(function () { adminRefsLoading = false; });
  }

  // --- форма «Добавить рейс» ---

  var creating = false;

  function initAdmin() {
    document.querySelectorAll('#tab-admin [data-hint-for]').forEach(function (h) { h.dataset.default = h.textContent; });

    var today = todayIso();
    $('cf-dep-date').min = today;
    $('cf-arr-date').min = today;

    function digitsOnly(id, maxLen) {
      $(id).addEventListener('input', function () {
        var v = $(id).value.replace(/\D/g, '').slice(0, maxLen);
        if (v !== $(id).value) $(id).value = v;
      });
    }
    digitsOnly('cf-number', 4);
    digitsOnly('cf-seats', 6);
    $('cf-price').addEventListener('input', function () {
      var v = $('cf-price').value.replace(/[^\d\s.,\u00a0]/g, '');
      if (v !== $('cf-price').value) $('cf-price').value = v;
    });

    // Дата прилёта по умолчанию следует за датой вылета, пока её не меняли вручную.
    var arrDateTouched = false;
    $('cf-arr-date').addEventListener('input', function () { arrDateTouched = !!$('cf-arr-date').value; });
    $('cf-dep-date').addEventListener('input', function () {
      if (!arrDateTouched) $('cf-arr-date').value = $('cf-dep-date').value;
      $('cf-arr-date').min = $('cf-dep-date').value || today;
    });

    $('cf-origin').addEventListener('change', function () { syncAirportSelects('cf-origin'); });
    $('cf-destination').addEventListener('change', function () { syncAirportSelects('cf-destination'); });

    ['cf-airline', 'cf-number', 'cf-origin', 'cf-destination', 'cf-dep-date', 'cf-dep-time',
      'cf-arr-date', 'cf-arr-time', 'cf-seats', 'cf-price'].forEach(function (id) {
      $(id).addEventListener('input', updateCreatePreview);
      $(id).addEventListener('change', updateCreatePreview);
    });

    $('create-form').addEventListener('reset', function () {
      if (creating) return;
      arrDateTouched = false;
      setTimeout(function () {
        $('create-error').hidden = true;
        clear($('create-error'));
        syncAirportSelects();
        updateCreatePreview();
      }, 0);
    });
    $('create-form').addEventListener('submit', function (e) { e.preventDefault(); submitCreate(); });

    $('edit-load-form').addEventListener('submit', function (e) {
      e.preventDefault();
      loadEditFlight($('edit-id').value.trim());
    });
    $('edit-id').addEventListener('input', function () {
      var v = $('edit-id').value.replace(/\D/g, '');
      if (v !== $('edit-id').value) $('edit-id').value = v;
    });
  }

  /** Один и тот же аэропорт нельзя выбрать и в «Откуда», и в «Куда». */
  function syncAirportSelects(changed) {
    var o = $('cf-origin'), d = $('cf-destination');
    if (changed && o.value && o.value === d.value) {
      (changed === 'cf-origin' ? d : o).value = '';
    }
    Array.prototype.forEach.call(d.options, function (opt) { opt.disabled = !!opt.value && opt.value === o.value; });
    Array.prototype.forEach.call(o.options, function (opt) { opt.disabled = !!opt.value && opt.value === d.value; });
  }

  function tzNote(code, atMs) {
    var tz = airportTz(code);
    if (!code) return '';
    if (!tz) return 'Часовой пояс аэропорта неизвестен — время в поясе браузера (' + utcOffsetLabel(tzOffsetMinutes(undefined, atMs)) + ')';
    return 'Местное время: ' + utcOffsetLabel(tzOffsetMinutes(tz, atMs)) + ' (' + tz + ')';
  }

  /** Читает поля формы -> { values, errors: { hintId: [message, inputs] } }. */
  function readCreateForm() {
    var errors = {};
    var airline = $('cf-airline').value;
    var origin = $('cf-origin').value;
    var destination = $('cf-destination').value;
    var flightNo = parsePositiveInt($('cf-number').value, 9999);
    var seats = parsePositiveInt($('cf-seats').value, MAX_INT32);
    var priceMinor = parseRublesToMinor($('cf-price').value);
    var dep = localToUtc($('cf-dep-date').value, $('cf-dep-time').value, airportTz(origin));
    var arr = localToUtc($('cf-arr-date').value, $('cf-arr-time').value, airportTz(destination));

    if (!airline || !adminAirlines[airline]) errors['cf-airline'] = ['Выберите авиакомпанию'];
    if (flightNo === null) errors['cf-number'] = ['Номер рейса — целое число от 1 до 9999'];
    if (!origin || !adminAirports[origin]) errors['cf-origin'] = ['Выберите аэропорт вылета'];
    if (!destination || !adminAirports[destination]) errors['cf-destination'] = ['Выберите аэропорт прилёта'];
    else if (origin === destination) errors['cf-destination'] = ['Пункты вылета и прилёта совпадают'];

    var depInputs = ['cf-dep-date', 'cf-dep-time'], arrInputs = ['cf-arr-date', 'cf-arr-time'];
    if (!dep) errors['cf-dep-date'] = ['Укажите дату и время вылета', depInputs];
    else if (dep.gap) errors['cf-dep-date'] = ['Такого местного времени нет (перевод часов)', depInputs];
    else if (dep.getTime() <= Date.now()) errors['cf-dep-date'] = ['Время вылета уже прошло', depInputs];
    if (!arr) errors['cf-arr-date'] = ['Укажите дату и время прилёта', arrInputs];
    else if (arr.gap) errors['cf-arr-date'] = ['Такого местного времени нет (перевод часов)', arrInputs];
    else if (dep instanceof Date && arr.getTime() <= dep.getTime()) errors['cf-arr-date'] = ['Прилёт должен быть позже вылета', arrInputs];

    if (seats === null) errors['cf-seats'] = ['Количество мест — целое число больше 0'];
    if (priceMinor === null || priceMinor <= 0) errors['cf-price'] = ['Цена больше 0, не более 2 знаков после запятой: 5500 или 5500,50'];

    return {
      values: {
        airline: airline, flightNo: flightNo, origin: origin, destination: destination,
        dep: dep instanceof Date ? dep : null, arr: arr instanceof Date ? arr : null,
        seats: seats, priceMinor: priceMinor
      },
      errors: errors
    };
  }

  var CREATE_FIELDS = [
    ['cf-airline'], ['cf-number'], ['cf-origin'], ['cf-destination'],
    ['cf-dep-date', ['cf-dep-date', 'cf-dep-time']], ['cf-arr-date', ['cf-arr-date', 'cf-arr-time']],
    ['cf-seats'], ['cf-price']
  ];

  /** Живые подсказки: полный номер рейса, часовые пояса, длительность, цена в копейках. */
  function updateCreatePreview() {
    var r = readCreateForm(), v = r.values;
    var now = Date.now();

    CREATE_FIELDS.forEach(function (f) {
      var hint = adminHint(f[0]);
      // Ошибку, показанную при отправке, снимаем, как только поле исправлено.
      if (hint.classList.contains('is-error') && !r.errors[f[0]]) adminFieldReset(f[0], f[1]);
    });

    function setHint(id, text, inputs) {
      if (!adminHint(id).classList.contains('is-error')) adminFieldReset(id, inputs, text);
    }
    setHint('cf-number', v.airline && v.flightNo ? 'Полный номер: ' + v.airline + v.flightNo : adminHint('cf-number').dataset.default);
    setHint('cf-origin', tzNote(v.origin, v.dep ? v.dep.getTime() : now));
    setHint('cf-destination', tzNote(v.destination, v.arr ? v.arr.getTime() : now));
    setHint('cf-price', v.priceMinor ? '= ' + formatMoney({ amount_minor: v.priceMinor, currency: 'RUB' }) + ' (' + v.priceMinor + ' коп.)' : adminHint('cf-price').dataset.default);

    $('cf-dep-legend').firstChild.nodeValue = 'Вылет' + (v.origin ? ' из ' + v.origin + ' ' : ' ');
    $('cf-arr-legend').firstChild.nodeValue = 'Прилёт' + (v.destination ? ' в ' + v.destination + ' ' : ' ');

    var box = $('cf-duration');
    box.classList.remove('is-error');
    if (v.dep && v.arr) {
      var dur = fmtDuration(v.dep.toISOString(), v.arr.toISOString());
      var utc = 'UTC: ' + fmt(v.dep.toISOString(), 'UTC', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' }) +
        ' → ' + fmt(v.arr.toISOString(), 'UTC', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
      if (dur) box.textContent = 'В пути ' + dur + ' · ' + utc;
      else { box.classList.add('is-error'); box.textContent = 'Прилёт должен быть позже вылета · ' + utc; }
    } else {
      box.textContent = '';
    }
  }

  function submitCreate() {
    if (creating) return;
    if (!adminRefsLoaded) { loadAdminRefs(); return; }
    var r = readCreateForm(), v = r.values;
    $('create-error').hidden = true;
    clear($('create-error'));

    var first = null;
    CREATE_FIELDS.forEach(function (f) {
      var e = r.errors[f[0]];
      if (e) {
        adminFieldError(f[0], e[0], e[1] || f[1]);
        if (!first) first = (e[1] || f[1] || [f[0]])[0];
      }
    });
    if (first) {
      showCreateError(simpleError('Исправьте отмеченные поля.'));
      $(first).focus();
      return;
    }

    var body = {
      airline_code: v.airline,
      flight_no: v.flightNo,
      origin: v.origin,
      destination: v.destination,
      departure_time: isoZ(v.dep),
      arrival_time: isoZ(v.arr),
      total_seats: v.seats,
      price: { amount_minor: v.priceMinor, currency: 'RUB' }
    };

    creating = true;
    var release = busy($('create-submit'), 'Создаём…');
    $('create-reset').disabled = true;
    adminApi('/admin/flights', { method: 'POST', body: body })
      .then(function (f) {
        if (!f || f.id === undefined) throw new ApiError('BAD_RESPONSE', 201, 'В ответе нет созданного рейса');
        toast('Рейс ' + (f.flight_number || '') + ' создан');
        flightCache.set(String(f.id), Promise.resolve(f));
        refreshSearchIfRoute(f);
        renderCreated(f);
      })
      .catch(function (err) {
        showCreateError(errorAlert(err, null, ADMIN_ERRORS));
        if (err.code === 'ALREADY_EXISTS') adminFieldError('cf-number', 'Рейс ' + v.airline + v.flightNo + ' на эту дату (UTC) уже существует');
      })
      .finally(function () {
        creating = false;
        release();
        $('create-reset').disabled = false;
      });
  }

  function showCreateError(content) {
    var box = $('create-error');
    clear(box);
    append(box, content);
    box.hidden = false;
  }

  function refreshSearchIfRoute(f) {
    if (lastSearch && lastSearch.origin === f.origin && lastSearch.destination === f.destination) {
      runSearch(lastSearch, true);
    }
  }

  /** UTC-дата вылета: поиск фильтрует по дате вылета в UTC. */
  function departureDateUtc(f) {
    var d = parseDate(f.departure_time);
    return d ? d.toISOString().slice(0, 10) : '';
  }

  function canBeSearched(f) {
    var d = parseDate(f.departure_time);
    return f.status === 'SCHEDULED' && d && d.getTime() > Date.now();
  }

  function goSearch(f) {
    var date = departureDateUtc(f);
    $('origin').value = f.origin;
    $('destination').value = f.destination;
    updateHint('origin'); updateHint('destination');
    $('date').value = date;
    $('date-clear').hidden = !date;
    location.hash = '#search';
    runSearch({ origin: f.origin, destination: f.destination, date: date }, false);
    window.scrollTo(0, 0);
  }

  function searchButton(f) {
    return el('button', {
      type: 'button', class: 'btn btn--ghost btn--small', text: 'Найти в поиске',
      title: 'Поиск ' + f.origin + ' → ' + f.destination + ' на ' + departureDateUtc(f) + ' (дата вылета в UTC)',
      onclick: function () { goSearch(f); }
    });
  }

  function idHead(prefix, f) {
    return [el('span', { text: prefix }), el('code', { text: 'id ' + f.id }), flightStatusBadge(f.status)];
  }

  var createdFlight = null;

  /** quiet=true — обновить карточку без прокрутки (после изменения рейса в блоке ниже). */
  function renderCreated(f, quiet) {
    createdFlight = f;
    var list = $('create-result');
    clear(list);
    var card = flightCard(f, {
      showStatus: true,
      head: idHead('Создан рейс', f),
      actions: [
        canBeSearched(f) ? searchButton(f) : null,
        el('button', {
          type: 'button', class: 'btn btn--primary btn--small', text: 'Изменить',
          onclick: function () {
            $('edit-id').value = String(f.id);
            loadEditFlight(String(f.id));
            $('edit-load-form').scrollIntoView({ behavior: 'smooth', block: 'start' });
          }
        })
      ]
    });
    list.appendChild(card);
    if (!quiet) card.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  }

  // --- «Изменить рейс» ---

  var editLoading = false;
  var editSeq = 0;
  var editFlight = null;

  function loadEditFlight(id) {
    var status = $('edit-status'), list = $('edit-result');
    if (editLoading) return;
    if (!/^\d+$/.test(id) || Number(id) < 1) {
      setStatus(status, simpleError('Введите номер (id) рейса — положительное целое число.'));
      $('edit-id').focus();
      return;
    }
    var seq = ++editSeq;
    editLoading = true;
    var release = busy($('edit-load-submit'), 'Загрузка…');
    setStatus(status, null);
    skeletons(list, 1);
    getFlight(id, true)
      .then(function (f) {
        if (seq !== editSeq) return;
        renderEdit(f);
      })
      .catch(function (err) {
        if (seq !== editSeq) return;
        clear(list);
        editFlight = null;
        if (err.code === 'NOT_FOUND') {
          setStatus(status, el('div', { class: 'alert alert--error', role: 'alert' },
            el('div', { class: 'alert__title', text: 'Рейс с id ' + id + ' не найден.' }),
            err.message ? el('div', { class: 'alert__detail', text: err.message }) : null));
        } else {
          setStatus(status, errorAlert(err, function () { loadEditFlight(id); }, ADMIN_ERRORS));
        }
      })
      .finally(function () { editLoading = false; release(); });
  }

  function renderEdit(f, notice) {
    editFlight = f;
    setStatus($('edit-status'), null);
    var list = $('edit-result');
    clear(list);

    var currency = (f.price && f.price.currency) || 'RUB';
    var statusSelect = el('select', { id: 'ef-status', name: 'status' });
    var known = false;
    FLIGHT_STATUSES.forEach(function (p) {
      if (p[0] === f.status) known = true;
      statusSelect.appendChild(el('option', { value: p[0], text: p[1] + ' (' + p[0] + ')' }));
    });
    if (!known && f.status) statusSelect.appendChild(el('option', { value: f.status, text: f.status }));
    statusSelect.value = f.status || 'SCHEDULED';

    var priceInput = el('input', {
      id: 'ef-price', name: 'price', type: 'text', inputmode: 'decimal', maxlength: '20', spellcheck: 'false',
      value: f.price ? minorToInput(f.price.amount_minor) : ''
    });
    var priceHint = el('small', { class: 'field__hint', text: 'Текущая: ' + formatMoney(f.price) });
    var saveBtn = el('button', { type: 'submit', class: 'btn btn--primary', text: 'Сохранить' });
    var errorBox = el('div', { hidden: true });
    var danger = el('div', { class: 'admin-edit__danger' });
    var busyNow = false;

    statusSelect.addEventListener('change', function () { showError(null); });
    priceInput.addEventListener('input', function () {
      showError(null);
      var v = priceInput.value.replace(/[^\d\s.,\u00a0]/g, '');
      if (v !== priceInput.value) priceInput.value = v;
      priceInput.removeAttribute('aria-invalid');
      priceHint.classList.remove('is-error');
      var m = parseRublesToMinor(priceInput.value);
      priceHint.textContent = m ? '= ' + formatMoney({ amount_minor: m, currency: currency }) + ' (' + m + ' коп.)' : 'Текущая: ' + formatMoney(f.price);
    });

    function showError(content) {
      clear(errorBox);
      append(errorBox, content);
      errorBox.hidden = !content;
    }

    function update(body, button, label, onFail) {
      if (busyNow) return;
      busyNow = true;
      showError(null);
      var release = busy(button, label);
      var others = [saveBtn, statusSelect, priceInput].concat(Array.prototype.slice.call(danger.querySelectorAll('button')))
        .filter(function (n) { return n !== button && !n.disabled; });
      others.forEach(function (n) { n.disabled = true; });
      adminApi('/admin/flights/' + encodeURIComponent(f.id), { method: 'POST', body: body })
        .then(function (updated) {
          if (!updated || updated.id === undefined) throw new ApiError('BAD_RESPONSE', 200, 'В ответе нет рейса');
          toast('Рейс ' + (updated.flight_number || '') + ' обновлён');
          flightCache.set(String(updated.id), Promise.resolve(updated));
          refreshSearchIfRoute(updated);
          if (createdFlight && String(createdFlight.id) === String(updated.id)) renderCreated(updated, true);
          renderEdit(updated, 'Изменения сохранены.');
        })
        .catch(function (err) {
          busyNow = false;
          release();
          others.forEach(function (n) { n.disabled = false; });
          if (onFail) onFail();
          if (err.code === 'NOT_FOUND') {
            showError(el('div', { class: 'alert alert--error', role: 'alert' },
              el('div', { class: 'alert__title', text: 'Рейс с id ' + f.id + ' не найден.' }),
              err.message ? el('div', { class: 'alert__detail', text: err.message }) : null));
          } else {
            showError(errorAlert(err, null, ADMIN_ERRORS));
          }
        });
    }

    var form = el('form', { class: 'admin-edit__form', novalidate: true, autocomplete: 'off' },
      el('div', { class: 'field' }, el('label', { for: 'ef-status', text: 'Статус' }), statusSelect),
      el('div', { class: 'field' }, el('label', { for: 'ef-price', text: 'Цена за место, ' + currency }), priceInput, priceHint),
      saveBtn);
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      if (busyNow) return;
      var body = {};
      if (statusSelect.value !== f.status) body.status = statusSelect.value;
      var priceMinor = parseRublesToMinor(priceInput.value);
      if (priceMinor === null || priceMinor <= 0) {
        priceInput.setAttribute('aria-invalid', 'true');
        priceHint.classList.add('is-error');
        priceHint.textContent = 'Цена больше 0, не более 2 знаков после запятой';
        priceInput.focus();
        return;
      }
      if (!f.price || priceMinor !== f.price.amount_minor) body.price = { amount_minor: priceMinor, currency: currency };
      if (!Object.keys(body).length) {
        showError(el('div', { class: 'alert alert--warning', role: 'status', text: 'Изменений нет — нечего сохранять.' }));
        return;
      }
      update(body, saveBtn, 'Сохраняем…');
    });

    function renderDanger() {
      clear(danger);
      if (f.status === 'CANCELLED') return;
      danger.appendChild(el('button', {
        type: 'button', class: 'btn btn--danger-ghost btn--small', text: 'Отменить рейс',
        onclick: renderConfirm
      }));
    }

    function renderConfirm() {
      if (busyNow) return;
      showError(null);
      clear(danger);
      var no = el('button', { type: 'button', class: 'btn btn--ghost btn--small', text: 'Нет', onclick: renderDanger });
      var yes = el('button', { type: 'button', class: 'btn btn--danger btn--small', text: 'Да, отменить' });
      yes.addEventListener('click', function () {
        if (yes.disabled) return;
        update({ status: 'CANCELLED' }, yes, 'Отменяем…', renderDanger);
      });
      append(danger, [el('span', { class: 'booking__confirm', text: 'Отменить рейс ' + f.flight_number + '? Он пропадёт из поиска.' }), no, yes]);
      no.focus();
    }

    renderDanger();

    var extra = el('div', { class: 'admin-edit' },
      notice ? el('div', { class: 'alert alert--success', role: 'status', text: notice }) : null,
      form, errorBox, danger);

    list.appendChild(flightCard(f, {
      showStatus: true,
      head: idHead('Рейс', f),
      actions: canBeSearched(f) ? [searchButton(f)] : [],
      extra: extra
    }));
  }

  // ---------------------------------------------------------------------------

  function init() {
    initUser();
    initSearch();
    initBookingDialog();
    initBookings();
    initLookup();
    initAdmin();
    window.addEventListener('hashchange', showTab);
    showTab();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
