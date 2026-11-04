// Takes the device through a list of states. The driver operates the switches,
// waits for each new state to settle and records each change; it judges
// nothing: the caller tells it which seconds of the recording are not normal.

export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

/** Waits until check() is true; throws message, or what it returns, after timeoutMs. */
export async function until(check, timeoutMs, message) {
  const end = Date.now() + timeoutMs;
  while (!(await check())) {
    if (Date.now() > end) throw new Error(typeof message === 'function' ? message() : message);
    await sleep(250);
  }
}

// The driver waits for the switch of a change as long as the board's criteria
// allow for the change (limitSeconds, from the caller), and this much longer.
const CHANGE_MARGIN_MS = 5000;
// A change that no rule names has no limit; the driver waits this long for
// its switch.
const CHANGE_WITHOUT_RULE_MS = 180_000;

/** A state is settled when its seconds are normal for this long in a row. */
export const SETTLE_CONFIRM_SECONDS = 2;
/** The driver waits this long for a state to settle, from the time at which its change's switch is done. */
export const SETTLE_CAP_SECONDS = 60;

// The recorder samples once per second, and a sample reports the second that
// ends at its time.
const SAMPLE_MS = 1000;
// Two samples can be this much less than a second apart.
const SAMPLE_EARLY_MS = 100;

// Asks check() until it returns true or the time `end` has come. Returns
// true, or what check() returned last.
async function waitUntil(check, end) {
  for (;;) {
    const seen = await check();
    if (seen === true || Date.now() >= end) return seen;
    await sleep(250);
  }
}

/**
 * When the state after a change was settled. seconds: the samples of the
 * recording so far, in order, each with the measures that are not normal in
 * its second: [{ t, notNormal: [{ measure, value }], normal: [measure] }]. startedAt: the call
 * of the change's switch. switchedAt: when the switch was done.
 *
 * The state is settled at the start of the first samples after the switch
 * that are normal in a row and cover `confirm` seconds. A sample reports the
 * second before its time, so that start is one second before the first of
 * them, or the time of the switch when that is later. `normal` of a sample
 * has the measures that are known to be normal in it: inside their limit,
 * covered by a known failure, or without a rule in the sample's state. A
 * measure in neither list has no value in that second, as a counter of the
 * Mini that the recorder reads once in some seconds, or is outside its
 * limit for fewer seconds in a row than its rule's "failAfterSeconds".
 *
 * A measure whose newest value before the change was not normal, and that
 * has not been normal since, is not the change's: its own rule judges it,
 * and the state settles without it. Before the change means in a sample
 * that was written before the change began: a sample takes up to a second,
 * so one that began less than a second before the change can have what the
 * change did.
 *
 * Returns { settledAt, waitedFor, last }. settledAt is null while the
 * samples do not show a settled state. waitedFor has each measure that kept
 * a second after the switch from being normal, { <measure>: { seconds, last } }:
 * the number of such seconds and the last such value. last is the newest
 * second after the switch that was not normal, as text, or null.
 */
export function settledAt(seconds, startedAt, switchedAt, confirm = SETTLE_CONFIRM_SECONDS) {
  const carried = new Set();
  let normal = [];
  let last = null;
  const waitedFor = {};
  for (const second of seconds) {
    for (const measure of second.normal) carried.delete(measure);
    if (second.t <= startedAt - SAMPLE_MS) {
      for (const { measure } of second.notNormal) carried.add(measure);
      continue;
    }
    if (second.t < switchedAt) continue;
    const own = second.notNormal.filter(({ measure }) => !carried.has(measure));
    if (own.length) {
      normal = [];
      last = own.map(({ measure, value }) => `${measure} is ${Number(value.toFixed(1))}`).join(', ');
      for (const { measure, value } of own) waitedFor[measure] = { seconds: (waitedFor[measure]?.seconds ?? 0) + 1, last: value };
      continue;
    }
    normal.push(second);
    const from = Math.max(switchedAt, normal[0].t - SAMPLE_MS);
    if (second.t - from >= confirm * 1000 - SAMPLE_EARLY_MS) return { settledAt: from, waitedFor, last };
  }
  return { settledAt: null, waitedFor, last };
}

/**
 * list: [{ state: { <variable>: true, ... }, holdSeconds }]. state is the
 * object that the recorder reads; a variable is in it from the start of its
 * change. record() gets one row per change:
 *
 *   { change: { <variable>: <on> }, state, startedAt, switchSeconds, seconds, error }
 *
 * A change's switch is done when isOn() of its switch agrees and its
 * works() is true; switchSeconds is the time from the call of the switch
 * until then. The change is done when the new state is settled (settledAt()
 * above), and seconds is its settling time: from the call of the switch to
 * the start of the normal seconds. A row whose state waited for a measure
 * has waitedFor. state is the state from the start of the change.
 *
 * limitSeconds(change, state) is the longest that the change may take, or
 * undefined when no rule names it. A switch that is not done when that time
 * and the margin are over gets `unfinished` in its row, what the switch said
 * last, and seconds is the time until then; the driver does not wait for
 * that state to settle.
 *
 * secondsNotNormal(pending) returns the samples of the recording so far for
 * settledAt(). pending is the row of the change whose switch is done, with
 * the time of that as `t`. The driver asks until the state is settled or
 * SETTLE_CAP_SECONDS after the switch are over. A state that is not settled
 * then gets `unsettled` in its row, the measures that were not normal last,
 * and seconds is the time until then. Without secondsNotNormal the driver
 * does not wait for a state to settle: seconds is switchSeconds.
 *
 * After a switch that was not done, and after a state that did not settle,
 * the variable stays in the state and the drive goes on with the next
 * change: what the variable then does is in the samples.
 *
 * A switch can name the variables that it ends when it comes on (`ends`),
 * as an update ends a mount. When such a switch is done, the driver turns
 * off those of them that are on, one after the other, before it waits for
 * the state to settle. Each has its own row, with `endedBy` and the time of
 * its switch as seconds; the state settles once, for the change that ended
 * them, from the time at which the last of them is off.
 *
 * A switch that throws leaves the device in an unknown state: the row gets
 * the error, and the drive ends there. Returns false if it ended early.
 */
export async function drive(list, switches, state, record, stopped = () => false, limitSeconds = () => undefined, secondsNotNormal = null) {
  const names = Object.keys(switches);
  // Operates one switch and waits until it is done. Returns the row of the
  // change as it is without the settling of its state.
  const operate = async (name, on) => {
    const startedAt = Date.now();
    let error = null;
    let unfinished = null;
    state[name] = on;
    const limit = limitSeconds({ [name]: on }, { ...state });
    const end = startedAt + (limit === undefined ? CHANGE_WITHOUT_RULE_MS : limit * 1000 + CHANGE_MARGIN_MS);
    try {
      await (on ? switches[name].on() : switches[name].off());
      if ((await waitUntil(async () => (await switches[name].isOn()) === on, end)) !== true) {
        unfinished = `${name} is still ${on ? 'off' : 'on'}`;
      } else if (switches[name].works) {
        const seen = await waitUntil(() => switches[name].works(on), end);
        if (seen !== true) {
          unfinished = `${name} is ${on ? 'on' : 'off'} and does not work` + (typeof seen === 'string' ? `: ${seen}` : '');
        }
      }
    } catch (e) {
      error = e.message;
    }
    const seconds = (Date.now() - startedAt) / 1000;
    return { change: { [name]: on }, state: { ...state }, startedAt, seconds, error,
      ...(unfinished ? { unfinished } : error ? {} : { switchSeconds: seconds }) };
  };
  for (const step of list) {
    const turnOff = names.filter(name => state[name] && !step.state[name]).reverse();
    const turnOn = names.filter(name => !state[name] && step.state[name]);
    for (const name of [...turnOff, ...turnOn]) {
      const on = turnOn.includes(name);
      // A variable that an earlier change of this step has ended is off already.
      if (Boolean(state[name]) === on) continue;
      const row = await operate(name, on);
      if (row.error || row.unfinished) {
        record(row);
        if (row.error) return false;
        continue;
      }
      const switchedAt = row.startedAt + row.switchSeconds * 1000;
      // The state can settle from here on: when the switch is done and what
      // it ended is off.
      let from = switchedAt;
      for (const other of on ? names.filter(other => state[other] && switches[name].ends?.includes(other)).reverse() : []) {
        const ended = await operate(other, false);
        record({ ...ended, endedBy: name });
        // The change's row is in every recording, for what the change pauses.
        if (ended.error) {
          record(row);
          return false;
        }
        from = Date.now();
      }
      if (secondsNotNormal) {
        const capAt = from + SETTLE_CAP_SECONDS * 1000;
        let seen;
        for (;;) {
          seen = settledAt(await secondsNotNormal({ t: switchedAt, ...row }), row.startedAt, from);
          if (seen.settledAt !== null || Date.now() >= capAt) break;
          if (stopped()) {
            record(row);
            return false;
          }
          await sleep(250);
        }
        if (Object.keys(seen.waitedFor).length) row.waitedFor = seen.waitedFor;
        if (seen.settledAt === null) {
          row.seconds = (Date.now() - row.startedAt) / 1000;
          row.unsettled = seen.last ?? 'the recording has no seconds after the switch';
        } else {
          row.seconds = (seen.settledAt - row.startedAt) / 1000;
        }
      }
      record(row);
    }
    for (let held = 0; held < step.holdSeconds; held++) {
      if (stopped()) return false;
      await sleep(1000);
    }
  }
  return true;
}

/** The list without the states in which one of the named variables is on. */
export function withoutVariables(list, names) {
  return list.filter(step => !names.some(name => step.state[name]));
}

/** Throws if the list names a variable that has no switch or has no hold time. */
export function validateList(list, variables) {
  if (!Array.isArray(list) || list.length === 0) throw new Error('the list has no states');
  list.forEach((step, index) => {
    const unknown = Object.keys(step.state ?? {}).filter(name => !variables.includes(name));
    if (!step.state || unknown.length) throw new Error(`state ${index + 1}: unknown variable ${unknown.join(', ') || '(no state)'}`);
    if (!(step.holdSeconds >= 0)) throw new Error(`state ${index + 1}: holdSeconds is missing`);
  });
}
