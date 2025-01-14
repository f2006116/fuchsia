// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        cache::{Asset, AssetCache},
        collection::{Typeface, TypefaceCollection},
        font_info, manifest,
    },
    failure::{format_err, Error, ResultExt},
    fdio, fidl,
    fidl::encoding::{Decodable, OutOfLine},
    fidl_fuchsia_fonts as fonts, fidl_fuchsia_fonts_experimental as fonts_exp,
    fidl_fuchsia_fonts_ext::{FontFamilyInfoExt, RequestExt, TypefaceResponseExt},
    fidl_fuchsia_intl as intl, fidl_fuchsia_mem as mem,
    fuchsia_component::server::{ServiceFs, ServiceObj},
    futures::prelude::*,
    log,
    parking_lot::RwLock,
    std::{
        collections::BTreeMap,
        fs::File,
        path::{Path, PathBuf},
        sync::Arc,
    },
    unicase::UniCase,
};

/// Get a field out of a `TypefaceRequest`'s `query` field as a reference, or returns early with a
/// `failure::Error` if the query is missing.
macro_rules! query_field {
    ($request:ident, $field:ident) => {
        $request.query.as_ref().ok_or(format_err!("Missing query"))?.$field.as_ref()
    };
}

/// Stores the relationship between [`Asset`] paths and IDs.
/// Should be initialized by [`FontService`].
///
/// `path_to_id_map` and `id_to_path_map` form a bidirectional map, so this relation holds:
/// ```
/// assert_eq!(self.path_to_id_map.get(&path), Some(&id));
/// assert_eq!(self.id_to_path_map.get(&id), Some(&path));
/// ```
struct AssetCollection {
    /// Maps [`Asset`] path to ID.
    path_to_id_map: BTreeMap<PathBuf, u32>,
    /// Inverse of `path_to_id_map`.
    id_to_path_map: BTreeMap<u32, PathBuf>,
    /// Next ID to assign, autoincremented from 0.
    next_id: u32,
    cache: RwLock<AssetCache>,
}

/// Get `VMO` handle to the [`Asset`] at `path`.
/// TODO(seancuff): Use a typed error instead.
fn load_asset_to_vmo(path: &Path) -> Result<mem::Buffer, Error> {
    let file = File::open(path)?;
    let vmo = fdio::get_vmo_copy_from_file(&file)?;
    let size = file.metadata()?.len();
    Ok(mem::Buffer { vmo, size })
}

const CACHE_SIZE_BYTES: u64 = 4_000_000;

impl AssetCollection {
    fn new() -> AssetCollection {
        AssetCollection {
            path_to_id_map: BTreeMap::new(),
            id_to_path_map: BTreeMap::new(),
            next_id: 0,
            cache: RwLock::new(AssetCache::new(CACHE_SIZE_BYTES)),
        }
    }

    /// Add the [`Asset`] found at `path` to the collection and return its ID.
    /// If `path` is already in the collection, return the existing ID.
    ///
    /// TODO(seancuff): Switch to updating ID of existing entries. This would allow assets to be
    /// updated without restarting the service (e.g. installing a newer version of a file). Clients
    /// would need to check the ID of their currently-held asset against the response.
    fn add_or_get_asset_id(&mut self, path: &Path) -> u32 {
        if let Some(id) = self.path_to_id_map.get(&path.to_path_buf()) {
            return *id;
        }
        let id = self.next_id;
        self.id_to_path_map.insert(id, path.to_path_buf());
        self.path_to_id_map.insert(path.to_path_buf(), id);
        self.next_id += 1;
        id
    }

    /// Get a `Buffer` holding the `Vmo` for the [`Asset`] corresponding to `id`, using the cache
    /// if possible.
    fn get_asset(&self, id: u32) -> Result<mem::Buffer, Error> {
        if let Some(path) = self.id_to_path_map.get(&id) {
            let mut cache_writer = self.cache.write();
            let buf = match cache_writer.get(id) {
                Some(cached) => cached.buffer,
                None => {
                    cache_writer
                        .push(Asset {
                            id,
                            buffer: load_asset_to_vmo(path).with_context(|_| {
                                format!("Failed to load {}", path.to_string_lossy())
                            })?,
                        })
                        .buffer
                }
            };
            return Ok(buf);
        }
        Err(format_err!("No asset found with id {}", id))
    }
}

#[derive(Debug)]
struct FontFamily {
    name: String,
    faces: TypefaceCollection,
    generic_family: Option<fonts::GenericFontFamily>,
}

impl FontFamily {
    fn new(name: String, generic_family: Option<fonts::GenericFontFamily>) -> FontFamily {
        FontFamily { name, faces: TypefaceCollection::new(), generic_family }
    }
}

#[derive(Debug)]
enum FamilyOrAlias {
    Family(FontFamily),
    /// Represents an alias to a `Family` whose name is the associated [`UniCase`]`<`[`String`]`>`.
    Alias(UniCase<String>),
}

pub struct FontService {
    assets: AssetCollection,
    /// Maps the font family name from the manifest (`families.family`) to a FamilyOrAlias.
    families: BTreeMap<UniCase<String>, FamilyOrAlias>,
    fallback_collection: TypefaceCollection,
}

impl FontService {
    pub fn new() -> FontService {
        FontService {
            assets: AssetCollection::new(),
            families: BTreeMap::new(),
            fallback_collection: TypefaceCollection::new(),
        }
    }

    /// Verify that we have a reasonable font configuration and can start.
    pub fn check_can_start(&self) -> Result<(), Error> {
        if self.fallback_collection.is_empty() {
            return Err(format_err!("Need at least one fallback font"));
        }

        Ok(())
    }

    pub fn load_manifest(&mut self, manifest_path: &Path) -> Result<(), Error> {
        fx_vlog!(1, "Loading manifest {:?}", manifest_path);
        let manifest = manifest::FontsManifest::load_from_file(&manifest_path)?;
        self.add_fonts_from_manifest(manifest).with_context(|_| {
            format!("Failed to load fonts from {}", manifest_path.to_string_lossy())
        })?;

        Ok(())
    }

    fn add_fonts_from_manifest(
        &mut self,
        mut manifest: manifest::FontsManifest,
    ) -> Result<(), Error> {
        let font_info_loader = font_info::FontInfoLoader::new()?;

        for mut family_manifest in manifest.families.drain(..) {
            if family_manifest.fonts.is_empty() {
                continue;
            }

            let family_name = UniCase::new(family_manifest.family.clone());

            // Get the [`FamilyOrAlias`] from `families` associated with `family_name`.
            //
            // If the key `family_name` does not exist in `families`, insert it with a
            // [`FamilyOrAlias`]`::Family` value.
            //
            // If a [`FamilyOrAlias`]`::Alias` with the same name already exists, [`Error`].
            let family = match self.families.entry(family_name.clone()).or_insert_with(|| {
                FamilyOrAlias::Family(FontFamily::new(
                    family_manifest.family.clone(),
                    family_manifest.generic_family,
                ))
            }) {
                FamilyOrAlias::Family(f) => f,
                FamilyOrAlias::Alias(other_family) => {
                    // Different manifest files may declare fonts for the same family,
                    // but font aliases cannot conflict with main family name.
                    return Err(format_err!(
                        "Conflicting font alias: {} is already declared as an alias for {}",
                        family_name,
                        other_family
                    ));
                }
            };

            for font_manifest in family_manifest.fonts.drain(..) {
                let asset_id = self.assets.add_or_get_asset_id(font_manifest.asset.as_path());

                let buffer = self.assets.get_asset(asset_id).with_context(|_| {
                    format!("Failed to load font from {}", font_manifest.asset.to_string_lossy())
                })?;

                let info = font_info_loader
                    .load_font_info(buffer.vmo, buffer.size as usize, font_manifest.index)
                    .with_context(|_| {
                        format!(
                            "Failed to load font info from {}",
                            font_manifest.asset.to_string_lossy()
                        )
                    })?;
                let typeface = Arc::new(Typeface::new(
                    asset_id,
                    font_manifest,
                    info.char_set,
                    family_manifest.generic_family,
                ));
                family.faces.add_typeface(typeface.clone());
                if family_manifest.fallback {
                    self.fallback_collection.add_typeface(typeface);
                }
            }

            // Register family aliases.
            for alias in family_manifest.aliases.unwrap_or(vec![]) {
                let alias_unicase = UniCase::new(alias.clone());

                match self.families.get(&alias_unicase) {
                    None => {
                        self.families
                            .insert(alias_unicase, FamilyOrAlias::Alias(family_name.clone()));
                    }
                    Some(FamilyOrAlias::Family(_)) => {
                        return Err(format_err!(
                            "Can't add alias {} for {} because a family with that name already \
                             exists.",
                            alias,
                            family_name
                        ))
                    }
                    Some(FamilyOrAlias::Alias(other_family)) => {
                        // If the alias exists then it must be for the same font family.
                        if *other_family != family_name {
                            return Err(format_err!(
                                "Can't add alias {} for {} because it's already declared as alias \
                                 for {}.",
                                alias,
                                family_name,
                                other_family
                            ));
                        }
                    }
                }
            }
        }
        Ok(())
    }

    /// Get font family by name.
    fn match_family(&self, family_name: &UniCase<String>) -> Option<&FontFamily> {
        let family = match self.families.get(family_name)? {
            FamilyOrAlias::Family(f) => f,
            FamilyOrAlias::Alias(a) => match self.families.get(a) {
                Some(FamilyOrAlias::Family(f)) => f,
                _ => panic!("Invalid font alias."),
            },
        };
        Some(family)
    }

    fn match_request(
        &self,
        mut request: fonts::TypefaceRequest,
    ) -> Result<fonts::TypefaceResponse, Error> {
        let matched_family: Option<&FontFamily> = query_field!(request, family)
            .and_then(|family| self.match_family(&UniCase::new(family.name.clone())));
        let mut typeface = match matched_family {
            Some(family) => family.faces.match_request(&request)?,
            None => None,
        };

        // If an exact match wasn't found, but fallback families are allowed...
        if typeface.is_none()
            && request
                .flags
                .map_or(true, |flags| !flags.contains(fonts::TypefaceRequestFlags::ExactFamily))
        {
            // If fallback_family is not specified by the client explicitly then copy it from
            // the matched font family.
            if query_field!(request, fallback_family).is_none() {
                if let Some(family) = matched_family {
                    request
                        .query
                        .as_mut()
                        .ok_or_else(|| format_err!("This should never happen"))?
                        .fallback_family = family.generic_family;
                }
            }
            typeface = self.fallback_collection.match_request(&request)?;
        }

        let typeface_response = typeface
            .ok_or("Couldn't match a typeface")
            .and_then(|font| match self.assets.get_asset(font.asset_id) {
                Ok(buffer) => Result::Ok(fonts::TypefaceResponse {
                    buffer: Some(buffer),
                    buffer_id: Some(font.asset_id),
                    font_index: Some(font.font_index),
                }),
                Err(err) => {
                    log::error!("Failed to load font file: {}", err);
                    Err("Failed to load font file")
                }
            })
            .unwrap_or_else(|_| fonts::TypefaceResponse::new_empty());

        // Note that not finding a typeface is not an error, as long as the query was legal.
        Ok(typeface_response)
    }

    fn get_family_info(&self, family_name: fonts::FamilyName) -> fonts::FontFamilyInfo {
        let family_name = UniCase::new(family_name.name);
        let family = self.match_family(&family_name);
        family.map_or_else(
            || fonts::FontFamilyInfo::new_empty(),
            |family| fonts::FontFamilyInfo {
                name: Some(fonts::FamilyName { name: family.name.clone() }),
                styles: Some(family.faces.get_styles().collect()),
            },
        )
    }

    fn get_typeface_by_id(&self, id: u32) -> Result<fonts::TypefaceResponse, fonts_exp::Error> {
        match self.assets.get_asset(id) {
            Ok(buffer) => {
                let response = fonts::TypefaceResponse {
                    buffer: Some(buffer),
                    buffer_id: Some(id),
                    font_index: None,
                };
                Ok(response)
            }
            Err(e) => {
                let msg = e.as_fail().to_string();
                if msg.starts_with("No asset found") {
                    return Err(fonts_exp::Error::NotFound);
                }
                Err(fonts_exp::Error::Internal)
            }
        }
    }

    fn get_typefaces_by_family(
        &self,
        family_name: fonts::FamilyName,
    ) -> Result<fonts_exp::TypefaceInfoResponse, fonts_exp::Error> {
        let family = self
            .match_family(&UniCase::new(family_name.name.clone()))
            .ok_or(fonts_exp::Error::NotFound)?;
        // Convert Vec<Arc<Typeface>> to Vec<TypefaceInfo>
        let faces: Vec<fonts_exp::TypefaceInfo> = family
            .faces
            .faces
            .iter()
            // Copy most fields from `Typeface` and use the canonical family name
            .map(|face| typefaceinfo_from_typeface(face, family.name.clone()))
            .collect();
        let response = fonts_exp::TypefaceInfoResponse { results: Some(faces) };
        Ok(response)
    }

    async fn handle_font_provider_request(
        &self,
        request: fonts::ProviderRequest,
    ) -> Result<(), failure::Error> {
        match request {
            // TODO(I18N-12): Remove when all clients have migrated to GetTypeface
            fonts::ProviderRequest::GetFont { request, responder } => {
                let request = request.into_typeface_request();
                let mut response = self.match_request(request)?.into_font_response();
                Ok(responder.send(response.as_mut().map(OutOfLine))?)
            }
            // TODO(I18N-12): Remove when all clients have migrated to GetFontFamilyInfo
            fonts::ProviderRequest::GetFamilyInfo { family, responder } => {
                let mut font_info =
                    self.get_family_info(fonts::FamilyName { name: family }).into_family_info();
                Ok(responder.send(font_info.as_mut().map(OutOfLine))?)
            }
            fonts::ProviderRequest::GetTypeface { request, responder } => {
                let response = self.match_request(request)?;
                // TODO(kpozin): OutOfLine?
                Ok(responder.send(response)?)
            }
            fonts::ProviderRequest::GetFontFamilyInfo { family, responder } => {
                let family_info = self.get_family_info(family);
                // TODO(kpozin): OutOfLine?
                Ok(responder.send(family_info)?)
            }
        }
    }

    #[allow(unused_variables)]
    async fn handle_experimental_request(
        &self,
        request: fonts_exp::ProviderRequest,
    ) -> Result<(), Error> {
        match request {
            fonts_exp::ProviderRequest::GetTypefaceById { id, responder } => {
                let mut response = self.get_typeface_by_id(id);
                Ok(responder.send(&mut response)?)
            }
            fonts_exp::ProviderRequest::GetTypefacesByFamily { family, responder } => {
                let mut response = self.get_typefaces_by_family(family);
                Ok(responder.send(&mut response)?)
            }
            fonts_exp::ProviderRequest::ListTypefaces { request, responder } => {
                Err(format_err!("Unimplemented: ListTypefaces"))
            }
        }
    }

    pub async fn run(self, fs: ServiceFs<ServiceObj<'static, ProviderRequestStream>>) {
        let self_ = Arc::new(self);
        await!(fs.for_each_concurrent(None, move |stream| self_.clone().handle_stream(stream)));
    }

    async fn handle_stream(self: Arc<Self>, stream: ProviderRequestStream) {
        let self_ = self.clone();
        match stream {
            ProviderRequestStream::Stable(stream) => {
                await!(self_.as_ref().handle_stream_stable(stream)).unwrap_or_default()
            }
            ProviderRequestStream::Experimental(stream) => {
                await!(self_.as_ref().handle_stream_experimental(stream)).unwrap_or_default()
            }
        }
    }

    async fn handle_stream_stable(
        &self,
        mut stream: fonts::ProviderRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = await!(stream.try_next()).context("Error running provider")? {
            await!(self.handle_font_provider_request(request))
                .context("Error while handling request")?;
        }
        Ok(())
    }

    async fn handle_stream_experimental(
        &self,
        mut stream: fonts_exp::ProviderRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = await!(stream.try_next()).context("Error running provider")? {
            await!(self.handle_experimental_request(request))
                .context("Error while handling request")?;
        }
        Ok(())
    }
}

pub enum ProviderRequestStream {
    Stable(fonts::ProviderRequestStream),
    Experimental(fonts_exp::ProviderRequestStream),
}

fn typefaceinfo_from_typeface(face: &Typeface, family: String) -> fonts_exp::TypefaceInfo {
    fonts_exp::TypefaceInfo {
        asset_id: Some(face.asset_id),
        font_index: Some(face.font_index),
        family: Some(fonts::FamilyName { name: family }),
        style: Some(fonts::Style2 {
            slant: Some(face.slant),
            weight: Some(face.weight),
            width: Some(face.width),
        }),
        languages: Some(
            // Convert BTreeSet<String> to Vec<LocaleId>
            face.languages.iter().map(|lang| intl::LocaleId { id: lang.clone() }).collect(),
        ),
        generic_family: face.generic_family,
    }
}
