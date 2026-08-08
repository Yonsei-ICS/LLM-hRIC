<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Contributing to OpenAirInterface

We want to make contributing to this project as easy and transparent as possible.

Please refer to the steps described on our website: [How to contribute to OAI](https://www.openairinterface.org/?page_id=112).

1. Sign and return a Contributor License Agreement to OAI team.
2. Register on [Eurecom GitLab Server](https://gitlab.eurecom.fr/users/sign_up)
   if you do not have any.
   - We recommend that you register with a professional or student email address.
   - If your email domain (`@domain.com`) is not whitelisted, please contact us
     (mailto:oaicicdteam@openairinterface.org).
   - Eurecom GitLab does NOT accept public email domains.
3. Provide the OAI team with the **username** of this account to
   (mailto:oaicicdteam@openairinterface.org) ; we will give you the developer
   rights on this repository.
4. The contributing policies are described in the [corresponding documentation
   page](doc/code-style-contrib.md).
   - PLEASE DO NOT FORK the OAI repository on your own Eurecom GitLab account.
     It just eats up space on our servers.
   - You can fork onto another hosting system. But we will NOT accept a merge
     request from a forked repository.
      * This decision was made for the license reasons.
      * The Continuous Integration will reject your merge request.
5. Mandatory signing of all the commits using the email address used for CLA.

## Commit Guidelines

### Signing Commits

To sign commits:

You can also get the verified label
on your commits via using [SSH KEYS or GPG KEYS](https://docs.gitlab.com/user/project/repository/signed_commits/)

```
# Edit .git/config in the git repository you are working on
# Add the user section
[user]
    name = YOUR NAME
    email = YOUR EMAIL ADDRESS

# If you use a signing key, use the below configuration instead
[user]
    name = YOUR NAME
    email = YOUR EMAIL ADDRESS
    signingkey = LOCATION OF SSH KEYS or GPG KEY

[gpg]
    format = ssh

[commit]
    gpgsign = true
```

> **NOTE:** If your commits are not signed the CI framework will not accept the MR.

For more information regarding contribution guidelines
please check [this document](doc/code-style-contrib.md)

## License

By contributing to OpenAirInterface, you agree that your contributions will be
licensed under

1. [CSSL v1.0 license](LICENSES/preferred/CSSL-v1.0.txt): for RAN and UE
   related source code and test scripts
2. [CC-BY-4.0](LICENSES/preferred/CC-BY-4.0.txt): All the documentation
3. [MIT](LICENSES/preferred/MIT.txt): Orchestration (helm-charts, docker
   compose)

Certain files are using different licenses; you can read about them in
[NOTICE](NOTICE).
