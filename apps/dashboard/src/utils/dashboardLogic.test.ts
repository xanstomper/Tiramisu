import test from 'node:test';
import assert from 'node:assert/strict';
import {
  INITIAL_GAMES,
  calculateStorageMetrics,
  stepCardSelection,
  filterGamesByQuery,
} from './dashboardLogic';

test('calculateStorageMetrics sums installed games to match 412.6 GB / 1.0 TB baseline', () => {
  // The 7 installed games (42.8 + 25.4 + 18.6 + 119.4 + 94.2 + 61.5 + 50.7) sum to 412.6 GB!
  const metrics = calculateStorageMetrics(INITIAL_GAMES, 0.0, 1.0);
  assert.equal(metrics.usedGb, 412.6);
  assert.equal(metrics.totalTb, '1.0');
  assert.equal(metrics.percentage, 41.3);
  assert.equal(metrics.freeGb, 587.4);
});

test('stepCardSelection wraps around cleanly for Xbox D-pad and bumper navigation', () => {
  assert.equal(stepCardSelection(0, 5, 1), 1);
  assert.equal(stepCardSelection(4, 5, 1), 0);
  assert.equal(stepCardSelection(0, 5, -1), 4);
});

test('filterGamesByQuery filters across title, drive location, and Title ID', () => {
  const gow = filterGamesByQuery(INITIAL_GAMES, 'God of War');
  assert.equal(gow.length, 1);
  assert.equal(gow[0].id, 'gow3');

  const internalGames = filterGamesByQuery(INITIAL_GAMES, '', 'internal');
  assert.equal(internalGames.length, 5);
});
