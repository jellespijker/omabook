<!-- Troubleshooting: https://jellespijker.github.io/omabook/help/troubleshooting.html -->

# Troubleshooting

**The AI page says a model is missing**

Press Test on the row. Omabook looks at what the server has and offers one that can do the job, or a small known one to download with one press.

**A Kindle or Word file opens slowly the first time**

Omabook converts it to EPUB once, with Calibre’s ebook-convert. The original stays beside the result.

**Nothing happens when I dictate**

Dictation listens only after you press it, and not at all with AI off. Check the Speech to text row’s Test.

**The graph is empty**

It grows as you finish chapters. Set it to “only when you ask” and run omabook graph build, or check that AI is on.

Still stuck? [Open an issue](https://github.com/jellespijker/omabook/issues). Failures are shown with a code and worded where you see them.
