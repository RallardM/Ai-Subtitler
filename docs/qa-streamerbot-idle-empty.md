# QA Test Plan: Streamer Bot Idle Empty String Logic

## Objective
Verify that after sending a sentence to Streamer Bot, if no new sentence is sent within 6 seconds, an empty string is sent once, and not repeatedly until a new sentence is sent.

## Steps
1. Start Ai-Subtitler with Streamer Bot integration enabled (use a test or mock endpoint if possible).
2. Send a test sentence (e.g., "Hello world") to Streamer Bot.
3. Wait for 7 seconds (longer than the idle timeout).
4. Observe that an empty string is sent to Streamer Bot after the 6-second idle period.
5. Wait another 7 seconds without sending any new sentence.
6. Confirm that no additional empty string is sent during continued inactivity.
7. Send another sentence (e.g., "Second message").
8. Wait 7 seconds again.
9. Confirm that another empty string is sent after this new idle period.

## Expected Results
- An empty string is sent only once after each idle period following a real sentence.
- No duplicate empty strings are sent during continuous inactivity.
- The logic resets after a new sentence is sent.

## Automation Suggestion
- Use a mock or test Streamer Bot endpoint that logs all received messages with timestamps.
- Script the above steps and check the log for correct message timing and content.

---

# Repair Plan (if test fails)
- If empty strings are sent repeatedly: Check the `m_sent_empty_since_last` logic in `streamerbot_sender`.
- If no empty string is sent after idle: Ensure the idle timer and condition are correctly implemented and not blocked by other logic.
- If empty string is sent too early/late: Verify the timeout duration and thread sleep/wait logic.
- If a new sentence does not reset the logic: Ensure `m_sent_empty_since_last` is reset on every enqueue.
