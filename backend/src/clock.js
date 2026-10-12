// Matches the explicit IANA/POSIX mapping in the no-PSRAM firmware settings.
export const CLOCK_TIMEZONES = Object.freeze(['Asia/Kolkata', 'Etc/UTC', 'Asia/Dubai',
  'Asia/Singapore', 'Asia/Tokyo', 'Europe/London', 'America/New_York', 'America/Los_Angeles']);
export function currentClockTime(timezone = 'Asia/Kolkata', now = new Date()) {
  const zone = CLOCK_TIMEZONES.includes(timezone) ? timezone : 'Asia/Kolkata';
  const local = new Intl.DateTimeFormat('en-IN', {
    timeZone: zone, dateStyle: 'full', timeStyle: 'long',
  }).format(now);
  const spoken = new Intl.DateTimeFormat('en-US', {
    timeZone: zone, hour: 'numeric', minute: '2-digit', hour12: true,
  }).format(now);
  return { utc: now.toISOString(), timezone: zone, local, spoken,
    ...(zone === 'Asia/Kolkata' ? { india: local } : {}) };
}
export function clockInstructions(timezone) {
  const zone = CLOCK_TIMEZONES.includes(timezone) ? timezone : 'Asia/Kolkata';
  return `\nThe device clock timezone is ${zone}. For current-time questions and explicit clock-display requests (show time, show me the time, open clock, display clock, show the clock), always call get_current_time and answer briefly using its spoken field, e.g. "It's 10:28 PM." The device itself opens the clock from the user's transcript and keeps it visible through your speech. Do not invent the time or claim failure to show this clock. Timers, alarms, reminders and unrelated mentions of time are separate requests and must not be treated as clock-display commands. Use the configured local timezone unless the user explicitly asks for another location.`;
}
