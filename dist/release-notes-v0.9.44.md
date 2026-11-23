## Hancho v0.9.44

### Changes
- **Failure classifier fix**: a plain (non-LLM) job whose output merely contains a permission-like identifier, such as the job JSON key `"on_permission_denied"`, is no longer classified as `permission_denied` and parked in `waitingForPermit`; it now fails normally so behavior trees take their fallback branch. The log-line permission rule matches whole words only and applies only to LLM worker jobs.

### 変更（日本語）
- **失敗分類の修正**: 普通の（LLM でない）ジョブの出力に、ジョブ JSON のキー `"on_permission_denied"` のような権限らしい識別子が含まれているだけで、`permission_denied` と判定されて `waitingForPermit` で止まる不具合を直しました。今は通常の失敗として扱われ、behavior tree のフォールバックの枝に進みます。ログ行による権限拒否の判定は単語単位で照合し、LLM ワーカーのジョブだけに適用します。
