/*
 * Get dependencies data out of a package.json
 */
let getOpam = name => {
  let ln = 6;
  if (String.length(name) > ln && String.sub(name, 0, ln) == "@opam/") {
    Some
      (name);
      /* Some(String.sub(name, ln, String.length(name) - ln)) */
  } else {
    None;
  };
};

let isGithub = value =>
  Str.string_match(
    Str.regexp("[a-zA-Z][a-zA-Z0-9-]+/[a-zA-Z0-9_-]+(#.+)?"),
    value,
    0,
  );

let startsWith = (value, needle) =>
  String.length(value) > String.length(needle)
  && String.sub(value, 0, String.length(needle)) == needle;

module DependencyRequest = {
  type t = {
    name: string,
    req,
  }
  and req =
    | Npm(GenericVersion.range(NpmVersion.t))
    | Github(string, string, option(string)) /* user, repo, ref (branch/tag/commit) */
    | Opam(GenericVersion.range(Types.opamConcrete)) /* opam allows a bunch of weird stuff. for now I'm just doing semver */
    | Git(string)
    | LocalPath(EsyLib.Path.t);

  /* [@test */
  /*   [ */
  /*     ( */
  /*       "let-def/wall#8b47b5ce898f6b35d6cbf92aa12baadd52f05350", */
  /*       Some( */
  /*         Types.Github( */
  /*           "let-def", */
  /*           "wall", */
  /*           Some("8b47b5ce898f6b35d6cbf92aa12baadd52f05350"), */
  /*         ), */
  /*       ), */
  /*     ), */
  /*     ( */
  /*       "bsancousi/bsb-native#fast", */
  /*       Some(Types.Github("bsancousi", "bsb-native", Some("fast"))), */
  /*     ), */
  /*     ("v1.2.3", None), */
  /*   ] */
  /* ] */
  let parseGithubVersion = text => {
    let parts = Str.split(Str.regexp_string("/"), text);
    switch (parts) {
    | [org, rest] =>
      switch (Str.split(Str.regexp_string("#"), rest)) {
      | [repo, ref] => Some(Github(org, repo, Some(ref)))
      | [repo] => Some(Github(org, repo, None))
      | _ => None
      }
    | _ => None
    };
  };

  let to_yojson = ({name: _, req}) =>
    switch (req) {
    | Npm(version) =>
      GenericVersion.range_to_yojson(NpmVersion.to_yojson, version)
    | Github(name, repo, Some(ref)) =>
      `String(name ++ "/" ++ repo ++ "#" ++ ref)
    | Github(name, repo, None) => `String(name ++ "/" ++ repo)
    | Git(url) => `String(url)
    | LocalPath(path) => `String(Path.toString(path))
    | Opam(version) =>
      GenericVersion.range_to_yojson(Types.alpha_to_yojson, version)
    };

  let reqToString = req =>
    switch (req) {
    | Npm(version) => GenericVersion.view(NpmVersion.toString, version)
    | Github(name, repo, Some(ref)) => name ++ "/" ++ repo ++ "#" ++ ref
    | Github(name, repo, None) => name ++ "/" ++ repo
    | Git(url) => url
    | LocalPath(path) => Path.toString(path)
    | Opam(_version) => "opam version"
    };

  let make = (name, value) =>
    if (startsWith(value, ".") || startsWith(value, "/")) {
      {name, req: LocalPath(Path.v(value))};
    } else {
      switch (getOpam(name)) {
      | Some(name) => {
          name,
          req:
            switch (parseGithubVersion(value)) {
            | Some(gh) => gh
            | None => Opam(OpamConcrete.parseNpmRange(value))
            /* NpmVersion.parseRange(value) |> GenericVersion.map(Shared.Types.opamFromNpmConcrete) */
            },
        }
      | None => {
          name,
          req:
            switch (parseGithubVersion(value)) {
            | Some(gh) => gh
            | None =>
              if (startsWith(value, "git+")) {
                Git(value);
              } else {
                Npm(NpmVersion.parseRange(value));
              }
            },
        }
      };
    };
};

module Dependencies = {
  type t = list(DependencyRequest.t);

  let empty = [];

  let of_yojson = json => {
    open Result.Syntax;
    let request = ((name, json: Json.t)) => {
      let%bind value = Json.Parse.string(json);
      return(DependencyRequest.make(name, value));
    };
    let%bind items = Json.Parse.assoc(json);
    Result.List.map(~f=request, items);
  };

  let to_yojson = (deps: t) : Json.t => {
    let items =
      List.map(
        ({DependencyRequest.name, _} as req) => (
          name,
          DependencyRequest.to_yojson(req),
        ),
        deps,
      );
    `Assoc(items);
  };

  let merge = (a, b) => {
    let seen = {
      let f = (seen, {DependencyRequest.name, _}) =>
        StringSet.add(name, seen);
      List.fold_left(f, StringSet.empty, a);
    };
    let f = (a, item) =>
      if (StringSet.mem(item.DependencyRequest.name, seen)) {
        a;
      } else {
        [item, ...a];
      };
    List.fold_left(f, a, b);
  };
};

module DependenciesInfo = {
  [@deriving yojson({strict: false})]
  type t = {
    dependencies: [@default Dependencies.empty] Dependencies.t,
    buildDependencies: [@default Dependencies.empty] Dependencies.t,
    devDependencies: [@default Dependencies.empty] Dependencies.t,
  };
};

module ExportedEnv = {
  type scope = [ | `Global | `Local];

  type item = {
    name: string,
    value: string,
    scope,
  };

  type t = list(item);

  let empty = [];

  let scope_to_yojson =
    fun
    | `Global => `String("global")
    | `Local => `String("local");

  let scope_of_yojson = (json: Json.t) : result(scope, string) =>
    Result.Syntax.(
      switch (json) {
      | `String("global") => return(`Global)
      | `String("local") => return(`Local)
      | _ => error("invalid scope value")
      }
    );

  let of_yojson = json =>
    Result.Syntax.(
      {
        let%bind items = Json.Parse.assoc(json);
        Result.List.map(
          ~f=
            ((name, v)) =>
              switch (v) {
              | `String(value) => return({name, value, scope: `Global})
              | `Assoc(_) =>
                let%bind value = Json.Parse.field(~name="val", v);
                let%bind value = Json.Parse.string(value);
                let%bind scope = Json.Parse.field(~name="scope", v);
                let%bind scope = scope_of_yojson(scope);
                return({name, value, scope});
              | _ => error("env value should be a string or an object")
              },
          items,
        );
      }
    );

  let to_yojson = (items: t) : Json.t => {
    let items =
      List.map(
        ({name, value, scope}) => (
          name,
          `Assoc([
            ("val", `String(value)),
            ("scope", scope_to_yojson(scope)),
          ]),
        ),
        items,
      );
    `Assoc(items);
  };
};

let getSource = json =>
  switch (json) {
  | `Assoc(items) =>
    switch (List.assoc("dist", items)) {
    | exception Not_found =>
      print_endline(Yojson.Safe.pretty_to_string(json));
      failwith("No dist");
    | `Assoc(items) =>
      let archive =
        switch (List.assoc("tarball", items)) {
        | `String(archive) => archive
        | _ => failwith("Bad tarball")
        };
      let checksum =
        switch (List.assoc("shasum", items)) {
        | `String(checksum) => checksum
        | _ => failwith("Bad checksum")
        };
      Types.PendingSource.Archive(archive, Some(checksum));
    | _ => failwith("bad dist")
    }
  | _ => failwith("bad json manifest")
  };