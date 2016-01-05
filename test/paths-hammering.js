/* -------------------------------- REQUIRES -------------------------------- */

const _ = require('lodash');
const async = require('async');
const assert = require('assert');
const request = require('request');
const {EventEmitter} = require('events');
const {Request} = require('ripple-lib');
const {Remote} = require('ripple-lib');
const {Amount} = require('ripple-lib');
const {PathFind} = require('ripple-lib')._test;
const {makeSuite} = require('./declarative-suite');

/* -------------------------------- CONSTANTS ------------------------------- */

const CONFIG = {

  LEDGER_DUMP: 'ledger-full-' + '1000' + '000.json',
  START_OWN_SERVER: true,
  HTTP_URL: 'http://127.0.0.1:5005',
  MAX_REQUESTS: 1000,// maximum path requests to try
  CLOSE_LEDGERS: true,// every 2-4 seconds

  REQUESTERS_BASE_CONFIG: {
    // retry after ms timeout without update. 0 == no retry
    RETRY_AFTER: 1250,
    WAIT_FULL_REPLY: true,
    // 'close'|'dispose', close will path_find subcommand : close
    DISPOSE_OR_CLOSE: 'dispose',
    CONNECTION_DROP_FREQUENCY: 0, // 0 - 1.0
    CONNECTION_DROP_AFTER: 50, // ms
    CLOSE_FIRST: false
  },

  // [num requesters, type, config overide]
  REQUESTERS: [
    [5,  'ws', {RETRY_AFTER: 100}],
    [10, 'ws', {}],
    [5,  'ws', {CONNECTION_DROP_FREQUENCY: 0.5}],
    [3,  'http', {RETRY_AFTER: 5000}]
  ]
};

/* --------------------------------- LOGGING -------------------------------- */

function LOG(msg, obj) {
  // filterings ;)
  if (msg.match(/^util\.race|(ws\.(connect|disconnect))$/)) {
    return;
  }

  console.log(_.extend({msg}, obj));
}

/* ----------------------------- MONKEY BUSINESS ---------------------------- */

// Does exactly the same as `PathFind::close` except doesn't actually issue a
// {command: 'path_find', 'subcommand' : 'close'} request
PathFind.prototype.dispose = function () {
  this.removeAllListeners('update');
  this.removeAllListeners('error');
  this.on('error', _.noop); // stfu ;)
  if (this.remote._cur_path_find === this) {
    this.remote._cur_path_find = null;
  } else {
    // assert(false);
  }
  // Don't think need this, but ripple-lib may use it
  this.emit('end');
  this.emit('close');
};

PathFind.prototype.close = function () {
  assert(false);
  this.removeAllListeners('update');
  const req = this.remote.requestPathFindClose();
  // Without this patch the request will error and crash the suite
  req.on('error', (error) => LOG('close_error', {error}));
  req.broadcast().request();
  this.emit('end');
  this.emit('close');
};

/* --------------------------------- HELPERS -------------------------------- */

function getLedger(remote, cb) {
  remote.request_ledger({validated: true, full: true}, function (e, m) {
    assert.ifError(e);
    cb(m.ledger);
  })
}

function prettyJSON(o) {
  return JSON.stringify(o, undefined, 2);
}

function isEntryType(type, e) {
  return e.LedgerEntryType === type;
}

const isAccountRoot = _.partial(isEntryType, 'AccountRoot');
const isLine = _.partial(isEntryType, 'RippleState');

function negateValue(v) {
  if (v[0] === '-') {
    return v.slice(1);
  }
  if (v !== '0') {
    return '-' + v;
  }
  return v;
}

function potentialBalanceAdj(line) {
  if (isPositive(line.limit)) {
    const limit = Number(line.limit);
    const balance = Number(line.balance);
    return limit - balance;
  }
  return 0;
}

function isPositive(value) {
  return value !== '0' && value[0] !== '-';
}

function getOrCreateArray(obj, name) {
  if (Array.isArray(obj[name])) {
    return obj[name];
  } else {
    return (obj[name] = []);
  }
}

function shouldRandomlyDo(frequency) {
  return Math.random() > (1 - frequency);
}

function groupAccounts(state) {
  const accounts = _.indexBy(state.filter(isAccountRoot), 'Account');
  const lines = state.filter(isLine);

  lines.forEach((l) => {
    const low = l.LowLimit.issuer;
    const high = l.HighLimit.issuer;
    const currency = l.Balance.currency;
    const balance = l.Balance.value;
    const highBalance = negateValue(balance);

    getOrCreateArray(accounts[low], 'lines').push(
      {limit: l.LowLimit.value, currency, balance,
       counterparty: high});

    getOrCreateArray(accounts[high], 'lines').push(
      {limit: l.HighLimit.value, currency, balance: highBalance,
       counterparty: low});
  });

  return {
    accounts,
    withBalance: _.transform(accounts, (r, v, k ) => {
      if (_.any(v.lines, (l) => isPositive(l.balance))) {
        r[k] = v;
      }
    }),
    withBalancePotential: _.transform(accounts, (r, v, k ) => {
      const lines = _.filter(v.lines, (l) => potentialBalanceAdj(l) > 0);
      if (lines.length > 0) {
        r[k] = lines;
      }
    })
  };
}

function findPathRequests(state, max=CONFIG.MAX_REQUESTS) {
  const {withBalance, withBalancePotential} = groupAccounts(state);
  const requests = [];
  _.forOwn(withBalancePotential, (lines, dest) => {
    lines.forEach(line => {
      _.forOwn(withBalance, (srcAccount, srcID) => {
        if (requests.length < max) {
          const amount = {
            value: String(Math.min(0.1, potentialBalanceAdj(line))),
            issuer: dest,
            currency: line.currency
          };
          requests.push([srcID, dest, amount]);
        } else {
          return false;
        }
      })
    });
  });
  return requests;
}

function makeRemotes(like, n, onDone) {
  const remotes = _.fill(Array(n), like._servers[0]._opts)
      .map((opt) => {
        return new Remote({servers: [_.clone(opt)]})
      });
  async.parallel(
    remotes.map(remote => (cb) => remote.connect(cb)),
    () => {
      onDone(remotes);
  });
}

function closeLedger(remote, baseDelta=2000, randomDelta=2000) {
  setTimeout(() => {
    remote.ledgerAccept((e, m) => {
      LOG('ledger_close', m);
      closeLedger(remote, baseDelta, randomDelta);
    });
  }, baseDelta + (Math.random() * randomDelta));
}

function canBeOnlyOne(cb=(() => {})) {
  let called = false;
  // wrapping func
  return (name, func) => {
    // wrapper
    return (...args) => {
      if (!called) {
        LOG('util.race', {winner: name});
        called = true;
        func(...args);
        cb();
      }
    }
  };
}

class RequesterConfiguration {
  constructor(stack) {
    this.conf = _.extend({}, stack);
    LOG('conf.new', {conf: this.conf});
  }
  get(key) {
    return this.conf[key] !== undefined ?
           this.conf[key] :
           CONFIG.REQUESTERS_BASE_CONFIG[key];
  }
}

class RequestsQueue extends EventEmitter {
  constructor(requests) {
    super();
    this.requests = requests;
    this.total = requests.length;
    this.finished = 0;
    this.outstanding = 0;
  }

  claim() {
    if (this.requests.length) {
      this.outstanding++;
      return this.requests.pop();
    }
    return null;
  }

  finishOne() {
    this.finished++;
    this.outstanding--;
    if (this.finished == this.total/* && this.outstanding == 0*/) {
      this.emit('finished');
    }
  }
}

class RequesterBase {
  constructor(id, requestsQueue, conf) {
    conf = conf || new RequesterConfiguration({});
    assert(requestsQueue instanceof RequestsQueue);
    assert(typeof id === 'number');
    assert(conf instanceof RequesterConfiguration);
    this.requestsQueue = requestsQueue;
    this.id = id;
    this.conf = conf
  }

  setCurrentRequest() {
    if (!this.currentRequest) {
      const claimed = this.requestsQueue.claim();
      if (claimed) {
        this.currentRequest = claimed;
      }
    }
    return Boolean(this.currentRequest);
  }

  completeOne() {
    this.currentRequest = null;
    this.requestsQueue.finishOne();
  }
}

class HttpPathFindRequester extends RequesterBase {
  constructor(url, id, requestsQueue, conf) {
    super(id, requestsQueue, conf);
    assert(typeof url === 'string' && url.match(/^http/));
    this.url = url;
  }

  request() {
    if (!this.setCurrentRequest()) {
      return;
    }
    const req = this.currentRequest;
    const [source_account, destination_account, destination_amount] = req;

    const body = {
      method: 'ripple_path_find',
      params: [
        {source_account, destination_account, destination_amount}
      ]
    };

    const requestParams = {
      method: 'POST',
      json: true,
      url: this.url,
      body,
      timeout: this.conf.get('RETRY_AFTER')
    };

    // console.log('request_params', prettyJSON(requestParams));
    var req_ = request(requestParams, (err, resp, body) => {
      if (resp) {
        LOG('http.response',
            {err: Boolean(err), body, status: resp.statusCode});
      }
      if (!err && resp && resp.statusCode === 200) {
        this.completeOne();
      }
      this.request();
    });
  }
}

class WebsocketPathFindRequester  extends RequesterBase {
  constructor(remote, id, requestsQueue, conf) {
    super(id, requestsQueue, conf);
    assert(remote instanceof Remote);
    this.remote = remote;
    remote.on('connect', this.onConnect.bind(this));
    remote.on('disconnect', this.onDisconnect.bind(this));
    this.attempt = 0;
  }

  onDisconnect() {
    LOG('ws.disconnect', {id: this.id});
  }

  onConnect() {
    LOG('ws.connect', {id: this.id});
    this.request();
  }

  request() {
    if (!this.setCurrentRequest()) {
      return;
    }

    if (!this.remote.isConnected()) {
      return;
    }

    // LOG('attempt', {id: this.id, attempt: this.attempt});
    this.attempt++;

    const remote = this.remote;
    const conf = this.conf;
    const [src_account, dst_account, dst_amount] = this.currentRequest;
    const options = {src_account, dst_account, dst_amount};
    const steps = [];

    if (conf.get('CLOSE_FIRST')) {
      steps.push((cb) => {
        remote.requestPathFindClose(() => {
          LOG('ws.close_first', {remote: remote.remoteIndex});
          cb();
        });
      });
    }

    steps.push((cb) => {
      const pf = remote.path_find(options);
      assert(pf !== null, 'not disposing properly somewhere');
      const raceEntrant = canBeOnlyOne();
      const winRace = raceEntrant('response', _.noop);

      // pf.dispose will clear these handlers
      pf.on('error', ((error) => {
        LOG('ws.path_find_error', {error})
        winRace();
        pf[conf.get('DISPOSE_OR_CLOSE')]();
        cb(new Error(error));
      }));
      pf.on('update', ((update) => {
        if ((conf.get('WAIT_FULL_REPLY') && !update.full_reply)) {
          return;
        }
        // +1 as we haven't marked it complete yet, which is done at the
        this.completeOne();
        LOG('ws.finished', {finished: this.requestsQueue.finished,
                            id: this.id,
                         alternatives: update.alternatives.map((a) => {
                          return a.paths_computed.map(p => {
                            return p.length;
                          });
                         })});
        winRace();
        pf[conf.get('DISPOSE_OR_CLOSE')]();
        cb();
      }));

      if (shouldRandomlyDo(conf.get('CONNECTION_DROP_FREQUENCY'))) {
        setTimeout(raceEntrant('reconnect', () => {
          LOG('ws.reconnecting', {id: this.id})
          pf.dispose();
          remote.reconnect();
          // It may take some time for this to be set to false, but we want this
          // done now.
          remote._connected = false;
          cb(new Error('dropped_connection'));
        }), conf.get('CONNECTION_DROP_AFTER'));
      }
      if (conf.get('RETRY_AFTER')) {
        setTimeout(raceEntrant('impatience', () => {
          pf.dispose();
          LOG('ws.impatient_retry', {finished: this.requestsQueue.finished,
                                  id: this.id});
          cb(new Error('impatient'));
        }), conf.get('RETRY_AFTER'));
      }
    });

    async.waterfall(steps, (err) => {
      if (remote.isConnected()) {
        this.request();
      }
    });
  }
}

function sumRemotes(requestersConf) {
  let tot = 0;
  requestersConf.forEach(([n, requesterType]) => {
    if (requesterType === 'ws') {
      tot += n;
    }
  });
  return tot;
}

function makeRequesters(requestersConf, remotes, queue) {
  const requesters = [];
  let id = 0;

  requestersConf.forEach(([n, requesterType, config]) => {
    var cnf = new RequesterConfiguration(config);
    for (var i = 0; i < n; i++) {
      if (requesterType === 'ws') {
        const remote = remotes.pop();
        assert(remote);
        const requester = new WebsocketPathFindRequester(
                remote, id++, queue, cnf);
        requesters.push(requester);
      }
      if (requesterType === 'http') {
        requesters.push(new HttpPathFindRequester(
                CONFIG.HTTP_URL, id++, queue, cnf));
      }
    }
  });
  return requesters;
}

makeSuite('path_find', {dump: CONFIG.LEDGER_DUMP,
                        no_server: CONFIG.START_OWN_SERVER },
  {
    stress: function (remote, done) {
      this.timeout(0);
      var cnf = CONFIG.REQUESTERS;
      var remotesNeeded = sumRemotes(cnf);
      getLedger(remote, (ledger) => {
        CONFIG.CLOSE_LEDGERS && closeLedger(remote);

        makeRemotes(remote, remotesNeeded, (remotes) => {
          const state = ledger.accountState;
          const queue = new RequestsQueue(findPathRequests(state));
          queue.on('finished', () => done());
          makeRequesters(cnf, remotes, queue).forEach(r => r.request());
        });
      });
    }
  }
);
