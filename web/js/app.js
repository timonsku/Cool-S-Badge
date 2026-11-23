// Shared state. Only one operation talks to the badge at a time.

import { SmpClient } from './smp.js';
import { Badge } from './badge.js';
import { notify } from './dom.js';

class App extends EventTarget {
  smp = new SmpClient();
  badge = new Badge(this.smp);
  ready = false;
  busy = false;
  version = null;
  // set while a disconnect is expected, such as the restart that erases the storage
  restarting = false;
  needsPresses = false;
  images = [];

  #job = null;

  constructor() {
    super();
    this.smp.addEventListener('disconnect', () => {
      this.ready = false;
      this.changed();
    });
  }

  changed() {
    this.dispatchEvent(new Event('change'));
  }

  // Background work (previews) steps aside for anything the user starts.
  async run(task) {
    if (this.busy) return;
    this.busy = true;
    this.changed();
    try {
      await this.stopBackground();
      await task();
    } catch (error) {
      notify(error.message || String(error), 'error');
    } finally {
      this.busy = false;
      this.changed();
    }
  }

  background(task) {
    if (this.busy || this.#job || !this.ready) return;
    const controller = new AbortController();
    const job = { controller };
    this.#job = job;
    job.done = Promise.resolve()
      .then(() => task(controller.signal))
      .catch((error) => {
        if (!controller.signal.aborted) notify(error.message, 'error');
      })
      .finally(() => {
        this.#job = null;
        // it may have been asked to start again while it was still stopping
        if (controller.signal.aborted) this.changed();
      });
  }

  async stopBackground() {
    const job = this.#job;
    job?.controller.abort();
    await job?.done;
  }
}

export const app = new App();
