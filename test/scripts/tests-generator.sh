#!/bin/bash

render()
{
    local template="$(cat $1)"
    . "$2"
    eval "echo \"${template}\""
}

generate()
{
    for i in test/template/*.tpl
    do
        file="$(basename $i)"
        IFS='_' read -r connector_type rest <<< "$file"
        if [ ! -f "test/config/${connector_type}.config" ]; then
            echo "Config file for $connector_type connector not found!"
            exit 1
        fi
        generated_test=$(render "$i" "test/config/${connector_type}.config")
        echo "$generated_test" > "test/sql/${connector_type}_10_installation_test.sql"
        echo "$generated_test" > "test/expected/${connector_type}_10_installation_test.out"
    done
}

generate