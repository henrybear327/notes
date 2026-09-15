# Notes

Welcome!

## Installation

When cloning for the first time, please run `git submodule update --init --recursive`.

After that, you can run `hugo serve` to see the website rendered locally.

Please check the [official instructions](https://github.com/adityatelange/hugo-PaperMod/wiki/Installation) for more details.

### Set up

For the config, please check [this page](https://github.com/adityatelange/hugo-PaperMod/wiki/Variables) for all available options.

For other usages, please check the [demo blog posts](https://adityatelange.github.io/hugo-PaperMod/archives/).

## Post

To start a new post, please run `hugo new content posts/<filename>.md`.

To start a new diary post, please run `hugo new content --kind diary posts/diary/<filename>.md`.

The posts are all in the directory `content/posts`.

No need to build and push the artifacts anymore, CI will take care of it! ^_^

## Checks

Every pull request builds the site (drafts included) and runs markdownlint, typos, an offline link check, and actionlint; see `.github/workflows/ci.yml`.

External links are checked every Monday by `.github/workflows/links.yml`, and a failed run is reported by email.

Dependabot opens monthly pull requests for the GitHub Actions and the PaperMod submodule. Hugo is not covered: bump `HUGO_VERSION` in `.github/workflows/gh-pages.yml` by hand, and CI builds with the new version on that pull request.

To run the content checks locally:

```sh
npx markdownlint-cli2
typos
lychee --offline 'content/**/*.md' README.md AGENTS.md
```
