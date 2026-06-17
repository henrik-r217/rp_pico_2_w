https://git-scm.com/book/en/v2/Git-Branching-Basic-Branching-and-Merging
_____________________________
$ git checkout -b iss53
Switched to a new branch "iss53" and create it.

This is shorthand for:
$ git branch iss53
$ git checkout iss53

________________________________

Edit and commit:
$ emacs index.html
$ git commit -a -m 'Create new footer [issue 53]'

___________________

$ git checkout master
Switched to branch 'master'
______________________________


Next, you have a hotfix to make. Let’s create a hotfix branch on which to work until it’s completed:

$ git checkout -b hotfix
Switched to a new branch 'hotfix'
$ emacs index.html
$ git commit -a -m 'Fix broken email address'
[hotfix 1fb7853] Fix broken email address
 1 file changed, 2 insertions(+)
____________________________
$ git branch -d hotfix
Deleted branch hotfix (3a0874c).

Now you can switch back to your work-in-progress branch on issue #53 and continue working on it.

$ git checkout iss53
Switched to branch "iss53"
$ emacs index.html
$ git commit -a -m 'Finish the new footer [issue 53]'
[iss53 ad82d7a] Finish the new footer [issue 53]
1 file changed, 1 insertion(+)


